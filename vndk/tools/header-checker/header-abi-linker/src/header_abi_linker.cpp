// Copyright (C) 2016 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "header_abi_util.h"
#include "ir_representation.h"
#include "so_file_parser.h"
#include "version_script_parser.h"

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <stdlib.h>

static constexpr std::size_t kSourcesPerBatchThread = 7;

static llvm::cl::OptionCategory header_linker_category(
    "header-abi-linker options");

static llvm::cl::list<std::string> dump_files(
    llvm::cl::Positional, llvm::cl::desc("<dump-files>"), llvm::cl::Required,
    llvm::cl::cat(header_linker_category), llvm::cl::OneOrMore);

static llvm::cl::opt<std::string> linked_dump(
    "o", llvm::cl::desc("<linked dump>"), llvm::cl::Required,
    llvm::cl::cat(header_linker_category));

static llvm::cl::list<std::string> exported_header_dirs(
    "I", llvm::cl::desc("<export_include_dirs>"), llvm::cl::Prefix,
    llvm::cl::ZeroOrMore, llvm::cl::cat(header_linker_category));

static llvm::cl::opt<std::string> version_script(
    "v", llvm::cl::desc("<version_script>"), llvm::cl::Optional,
    llvm::cl::cat(header_linker_category));

static llvm::cl::opt<std::string> api(
    "api", llvm::cl::desc("<api>"), llvm::cl::Optional,
    llvm::cl::cat(header_linker_category));

static llvm::cl::opt<std::string> arch(
    "arch", llvm::cl::desc("<arch>"), llvm::cl::Optional,
    llvm::cl::cat(header_linker_category));

static llvm::cl::opt<bool> no_filter(
    "no-filter", llvm::cl::desc("Do not filter any abi"), llvm::cl::Optional,
    llvm::cl::cat(header_linker_category));

static llvm::cl::opt<std::string> so_file(
    "so", llvm::cl::desc("<path to so file>"), llvm::cl::Optional,
    llvm::cl::cat(header_linker_category));

static llvm::cl::opt<abi_util::TextFormatIR> input_format(
    "input-format", llvm::cl::desc("Specify format of input dump files"),
    llvm::cl::values(clEnumValN(abi_util::TextFormatIR::ProtobufTextFormat,
                                "ProtobufTextFormat", "ProtobufTextFormat"),
                     clEnumValN(abi_util::TextFormatIR::Json, "Json", "JSON")),
    llvm::cl::init(abi_util::TextFormatIR::Json),
    llvm::cl::cat(header_linker_category));

static llvm::cl::opt<abi_util::TextFormatIR> output_format(
    "output-format", llvm::cl::desc("Specify format of output dump file"),
    llvm::cl::values(clEnumValN(abi_util::TextFormatIR::ProtobufTextFormat,
                                "ProtobufTextFormat", "ProtobufTextFormat"),
                     clEnumValN(abi_util::TextFormatIR::Json, "Json", "JSON")),
    llvm::cl::init(abi_util::TextFormatIR::Json),
    llvm::cl::cat(header_linker_category));

class HeaderAbiLinker {
 public:
  HeaderAbiLinker(
      const std::vector<std::string> &dump_files,
      const std::vector<std::string> &exported_header_dirs,
      const std::string &version_script,
      const std::string &so_file,
      const std::string &linked_dump,
      const std::string &arch,
      const std::string &api)
      : dump_files_(dump_files), exported_header_dirs_(exported_header_dirs),
        version_script_(version_script), so_file_(so_file),
        out_dump_name_(linked_dump), arch_(arch), api_(api) {}

  bool LinkAndDump();

 private:
  template <typename T>
  bool LinkDecl(abi_util::IRDumper *dst,
                const abi_util::AbiElementMap<T> &src,
                const std::function<bool(const std::string &)> &symbol_filter);

  bool ParseVersionScriptFiles();

  bool ParseSoFile();

  bool LinkTypes(const abi_util::TextFormatToIRReader *ir_reader,
                 abi_util::IRDumper *ir_dumper);

  bool LinkFunctions(const abi_util::TextFormatToIRReader *ir_reader,
                     abi_util::IRDumper *ir_dumper);

  bool LinkGlobalVars(const abi_util::TextFormatToIRReader *ir_reader,
                      abi_util::IRDumper *ir_dumper);

  bool AddElfSymbols(abi_util::IRDumper *ir_dumper);


 private:
  const std::vector<std::string> &dump_files_;
  const std::vector<std::string> &exported_header_dirs_;
  const std::string &version_script_;
  const std::string &so_file_;
  const std::string &out_dump_name_;
  const std::string &arch_;
  const std::string &api_;
  // TODO: Add to a map of std::sets instead.
  std::set<std::string> exported_headers_;
  std::map<std::string, abi_util::ElfFunctionIR> function_decl_map_;
  std::map<std::string, abi_util::ElfObjectIR> globvar_decl_map_;
  // Version Script Regex Matching.
  std::set<std::string> functions_regex_matched_set;
  std::regex functions_vs_regex_;
  // Version Script Regex Matching.
  std::set<std::string> globvars_regex_matched_set;
  std::regex globvars_vs_regex_;
};

template <typename T>
static bool AddElfSymbols(abi_util::IRDumper *dst,
                          const std::map<std::string, T> &symbols) {
  for (auto &&symbol : symbols) {
    if (!dst->AddElfSymbolMessageIR(&(symbol.second))) {
      return false;
    }
  }
  return true;
}

// To be called right after parsing the .so file / version script.
bool HeaderAbiLinker::AddElfSymbols(abi_util::IRDumper *ir_dumper) {
  return ::AddElfSymbols(ir_dumper, function_decl_map_) &&
         ::AddElfSymbols(ir_dumper, globvar_decl_map_);
}

static void DeDuplicateAbiElementsThread(
    const std::vector<std::string> &dump_files,
    const std::set<std::string> *exported_headers,
    abi_util::TextFormatToIRReader *greader, std::mutex *greader_lock,
    std::atomic<std::size_t> *cnt) {
  std::unique_ptr<abi_util::TextFormatToIRReader> local_reader =
      abi_util::TextFormatToIRReader::CreateTextFormatToIRReader(
          input_format, exported_headers);
  auto begin_it = dump_files.begin();
  std::size_t num_sources = dump_files.size();
  while (1) {
    std::size_t i = cnt->fetch_add(kSourcesPerBatchThread);
    if (i >= num_sources) {
      break;
    }
    std::size_t end = std::min(i + kSourcesPerBatchThread, num_sources);
    for (auto it = begin_it; it != begin_it + end; it++) {
      std::unique_ptr<abi_util::TextFormatToIRReader> reader =
          abi_util::TextFormatToIRReader::CreateTextFormatToIRReader(
              input_format, exported_headers);
      assert(reader != nullptr);
      if (!reader->ReadDump(*it)) {
        llvm::errs() << "ReadDump failed\n";
        ::exit(1);
      }
      // This merge is needed since the iterators might not be contigous.
      local_reader->MergeGraphs(*reader);
    }
  }
  std::lock_guard<std::mutex> lock(*greader_lock);
  greader->MergeGraphs(*local_reader);
}

bool HeaderAbiLinker::LinkAndDump() {
  // If the user specifies that a version script should be used, use that.
  if (!so_file_.empty()) {
    exported_headers_ =
        abi_util::CollectAllExportedHeaders(exported_header_dirs_);
    if (!ParseSoFile()) {
      llvm::errs() << "Couldn't parse so file\n";
      return false;
    }
  } else if (!ParseVersionScriptFiles()) {
    llvm::errs() << "Failed to parse stub files for exported symbols\n";
    return false;
  }
  std::unique_ptr<abi_util::IRDumper> ir_dumper =
      abi_util::IRDumper::CreateIRDumper(output_format, out_dump_name_);
  assert(ir_dumper != nullptr);
  AddElfSymbols(ir_dumper.get());
  // Create a reader, on which we never actually call ReadDump(), since multiple
  // dump files are associated with it.
  std::unique_ptr<abi_util::TextFormatToIRReader> greader =
      abi_util::TextFormatToIRReader::CreateTextFormatToIRReader(
          input_format, &exported_headers_);
  std::size_t max_threads = std::thread::hardware_concurrency();
  std::size_t num_threads = kSourcesPerBatchThread < dump_files_.size() ?
                    std::min(dump_files_.size() / kSourcesPerBatchThread,
                             max_threads) : 0;
  std::vector<std::thread> threads;
  std::atomic<std::size_t> cnt(0);
  std::mutex greader_lock;
  for (std::size_t i = 1; i < num_threads; i++) {
    threads.emplace_back(DeDuplicateAbiElementsThread, dump_files_,
                         &exported_headers_, greader.get(), &greader_lock,
                         &cnt);
  }
  DeDuplicateAbiElementsThread(dump_files_, &exported_headers_, greader.get(),
                               &greader_lock, &cnt);
  for (auto &thread : threads) {
    thread.join();
  }

  if (!LinkTypes(greader.get(), ir_dumper.get()) ||
      !LinkFunctions(greader.get(), ir_dumper.get()) ||
      !LinkGlobalVars(greader.get(), ir_dumper.get())) {
    llvm::errs() << "Failed to link elements\n";
    return false;
  }
  if (!ir_dumper->Dump()) {
    llvm::errs() << "Serialization to ostream failed\n";
    return false;
  }
  return true;
}

static bool QueryRegexMatches(std::set<std::string> *regex_matched_link_set,
                              const std::regex *vs_regex,
                              const std::string &symbol) {
  assert(regex_matched_link_set != nullptr);
  assert(vs_regex != nullptr);
  if (regex_matched_link_set->find(symbol) != regex_matched_link_set->end()) {
    return false;
  }
  if (std::regex_search(symbol, *vs_regex)) {
    regex_matched_link_set->insert(symbol);
    return true;
  }
  return false;
}

static std::regex CreateRegexMatchExprFromSet(
    const std::set<std::string> &link_set) {
  std::string all_regex_match_str = "";
  std::set<std::string>::iterator it = link_set.begin();
  while (it != link_set.end()) {
    std::string regex_match_str_find_glob =
      abi_util::FindAndReplace(*it, "\\*", ".*");
    all_regex_match_str += "(\\b" + regex_match_str_find_glob + "\\b)";
    if (++it != link_set.end()) {
      all_regex_match_str += "|";
    }
  }
  if (all_regex_match_str == "") {
    return std::regex();
  }
  return std::regex(all_regex_match_str);
}

template <typename T>
bool HeaderAbiLinker::LinkDecl(
    abi_util::IRDumper *dst, const abi_util::AbiElementMap<T> &src,
    const std::function<bool(const std::string &)> &symbol_filter) {
  assert(dst != nullptr);
  for (auto &&element : src) {
    // If we are not using a version script and exported headers are available,
    // filter out unexported abi.
    std::string source_file = element.second.GetSourceFile();
    // Builtin types will not have source file information.
    if (!exported_headers_.empty() && !source_file.empty() &&
        exported_headers_.find(source_file) == exported_headers_.end()) {
      continue;
    }
    // Check for the existence of the element in version script / symbol file.
    if (!symbol_filter(element.first)) {
      continue;
    }
    if (!dst->AddLinkableMessageIR(&(element.second))) {
      llvm::errs() << "Failed to add element to linked dump\n";
      return false;
    }
  }
  return true;
}

bool HeaderAbiLinker::LinkTypes(const abi_util::TextFormatToIRReader *reader,
                                abi_util::IRDumper *ir_dumper) {
  assert(reader != nullptr);
  auto no_filter = [](const std::string &symbol) { return true; };
  return LinkDecl(ir_dumper, reader->GetRecordTypes(), no_filter) &&
         LinkDecl(ir_dumper, reader->GetEnumTypes(), no_filter) &&
         LinkDecl(ir_dumper, reader->GetFunctionTypes(), no_filter) &&
         LinkDecl(ir_dumper, reader->GetBuiltinTypes(), no_filter) &&
         LinkDecl(ir_dumper, reader->GetPointerTypes(), no_filter) &&
         LinkDecl(ir_dumper, reader->GetRvalueReferenceTypes(), no_filter) &&
         LinkDecl(ir_dumper, reader->GetLvalueReferenceTypes(), no_filter) &&
         LinkDecl(ir_dumper, reader->GetArrayTypes(), no_filter) &&
         LinkDecl(ir_dumper, reader->GetQualifiedTypes(), no_filter);
}

bool HeaderAbiLinker::LinkFunctions(
    const abi_util::TextFormatToIRReader *reader,
    abi_util::IRDumper *ir_dumper) {
  assert(reader != nullptr);
  auto symbol_filter = [this](const std::string &linker_set_key) {
    return function_decl_map_.find(linker_set_key) !=
               function_decl_map_.end() ||
           QueryRegexMatches(&functions_regex_matched_set, &functions_vs_regex_,
                             linker_set_key);
  };
  return LinkDecl(ir_dumper, reader->GetFunctions(), symbol_filter);
}

bool HeaderAbiLinker::LinkGlobalVars(
    const abi_util::TextFormatToIRReader *reader,
    abi_util::IRDumper *ir_dumper) {
  assert(reader != nullptr);
  auto symbol_filter = [this](const std::string &linker_set_key) {
    return globvar_decl_map_.find(linker_set_key) !=
               globvar_decl_map_.end() ||
           QueryRegexMatches(&globvars_regex_matched_set, &globvars_vs_regex_,
                             linker_set_key);
  };
  return LinkDecl(ir_dumper, reader->GetGlobalVariables(), symbol_filter);
}

bool HeaderAbiLinker::ParseVersionScriptFiles() {
  abi_util::VersionScriptParser version_script_parser(
      version_script_, arch_, api_);
  if (!version_script_parser.Parse()) {
    llvm::errs() << "Failed to parse version script\n";
    return false;
  }
  function_decl_map_ = version_script_parser.GetFunctions();
  globvar_decl_map_ = version_script_parser.GetGlobVars();
  std::set<std::string> function_regexs =
      version_script_parser.GetFunctionRegexs();
  std::set<std::string> globvar_regexs =
      version_script_parser.GetGlobVarRegexs();
  functions_vs_regex_ = CreateRegexMatchExprFromSet(function_regexs);
  globvars_vs_regex_ = CreateRegexMatchExprFromSet(globvar_regexs);
  return true;
}

bool HeaderAbiLinker::ParseSoFile() {
  auto Binary = llvm::object::createBinary(so_file_);

  if (!Binary) {
    llvm::errs() << "Couldn't really create object File \n";
    return false;
  }
  llvm::object::ObjectFile *objfile =
      llvm::dyn_cast<llvm::object::ObjectFile>(&(*Binary.get().getBinary()));
  if (!objfile) {
    llvm::errs() << "Not an object file\n";
    return false;
  }

  std::unique_ptr<abi_util::SoFileParser> so_parser =
      abi_util::SoFileParser::Create(objfile);
  if (so_parser == nullptr) {
    llvm::errs() << "Couldn't create soFile Parser\n";
    return false;
  }
  so_parser->GetSymbols();
  function_decl_map_ = so_parser->GetFunctions();
  globvar_decl_map_ = so_parser->GetGlobVars();
  return true;
}

// Hide irrelevant command line options defined in LLVM libraries.
static void HideIrrelevantCommandLineOptions() {
  llvm::StringMap<llvm::cl::Option *> &map = llvm::cl::getRegisteredOptions();
  for (llvm::StringMapEntry<llvm::cl::Option *> &p : map) {
    if (p.second->Category == &header_linker_category) {
      continue;
    }
    if (p.first().startswith("help")) {
      continue;
    }
    p.second->setHiddenFlag(llvm::cl::Hidden);
  }
}

int main(int argc, const char **argv) {
  HideIrrelevantCommandLineOptions();

  llvm::cl::ParseCommandLineOptions(argc, argv, "header-linker");
  if (so_file.empty() && version_script.empty()) {
    llvm::errs() << "One of -so or -v needs to be specified\n";
    return -1;
  }
  if (no_filter) {
    static_cast<std::vector<std::string> &>(exported_header_dirs).clear();
  }
  HeaderAbiLinker Linker(dump_files, exported_header_dirs, version_script,
                         so_file, linked_dump, arch, api);

  if (!Linker.LinkAndDump()) {
    llvm::errs() << "Failed to link and dump elements\n";
    return -1;
  }
  return 0;
}
