//==========================================================================
//  INIFILECONTENTS.CC - part of
//                     OMNeT++/OMNEST
//            Discrete System Simulation in C++
//
//==========================================================================

/*--------------------------------------------------------------*
  Copyright (C) 1992-2017 Andras Varga
  Copyright (C) 2006-2017 OpenSim Ltd.

  This file is distributed WITHOUT ANY WARRANTY. See the file
  `license' for details on this and other legal matters.
*--------------------------------------------------------------*/

#include <cassert>
#include <algorithm>
#include <sstream>
#include <fstream>
#include "common/opp_ctype.h"
#include "common/patternmatcher.h"
#include "common/stringtokenizer.h"
#include "common/stringutil.h"
#include "common/fileutil.h"
#include "common/stlutil.h"
#include "common/enumstr.h"
#include "omnetpp/cexception.h"
#include "omnetpp/globals.h"
#include "omnetpp/cconfigoption.h"
#include "omnetpp/platdep/platmisc.h"  // getpid()
#include "configuration.h"
#include "inifilereader.h"
#include "valueiterator.h"
#include "scenario.h"

using namespace omnetpp::common;
using namespace omnetpp::internal;

namespace omnetpp {

extern cConfigOption *CFGID_NETWORK;
extern cConfigOption *CFGID_SEED_SET;

namespace envir {

//TODO sectionId, entryId -> sectionIndex, entryIndex

// XXX make sure quoting "$\{" works!

Register_GlobalConfigOption(CFGID_DESCRIPTION, "description", CFG_STRING, nullptr, "Descriptive name for the given simulation configuration. Descriptions get displayed in the run selection dialog.");
Register_GlobalConfigOption(CFGID_EXTENDS, "extends", CFG_STRING, nullptr, "Name of the configuration this section is based on. Entries from that section will be inherited and can be overridden. In other words, configuration lookups will fall back to the base section.");
Register_GlobalConfigOption(CFGID_CONSTRAINT, "constraint", CFG_STRING, nullptr, "For scenarios. Contains an expression that iteration variables (`${}` syntax) must satisfy for that simulation to run. Example: `$i < $j+1`.");
Register_GlobalConfigOption(CFGID_ITERATION_NESTING_ORDER, "iteration-nesting-order", CFG_STRING, nullptr, "Specifies the loop nesting order for iteration variables (`${}` syntax). The value is a comma-separated list of iteration variables; the list may also contain at most one asterisk. Variables that are not explicitly listed will be inserted at the position of the asterisk, or appended to the list if there is no asterisk. The first variable will become the outermost loop, and the last one the innermost loop. Example: `repetition,numHosts,*,iaTime`.");
Register_GlobalConfigOption(CFGID_REPEAT, "repeat", CFG_INT, "1", "For scenarios. Specifies how many replications should be done with the same parameters (iteration variables). This is typically used to perform multiple runs with different random number seeds. The loop variable is available as `${repetition}`. See also: `seed-set` key.");
Register_GlobalConfigOption(CFGID_EXPERIMENT_LABEL, "experiment-label", CFG_STRING, "${configname}", "Identifies the simulation experiment (which consists of several, potentially repeated measurements). This string gets recorded into result files, and may be referred to during result analysis.");
Register_GlobalConfigOption(CFGID_MEASUREMENT_LABEL, "measurement-label", CFG_STRING, "${iterationvars}", "Identifies the measurement within the experiment. This string gets recorded into result files, and may be referred to during result analysis.");
Register_GlobalConfigOption(CFGID_REPLICATION_LABEL, "replication-label", CFG_STRING, "#${repetition}", "Identifies one replication of a measurement (see `repeat` and `measurement-label` options as well). This string gets recorded into result files, and may be referred to during result analysis.");
Register_GlobalConfigOption(CFGID_RUNNUMBER_WIDTH, "runnumber-width", CFG_INT, "0", "Setting a nonzero value will cause the `$runnumber` variable to get padded with leading zeroes to the given length.");
Register_GlobalConfigOption(CFGID_RESULT_DIR, "result-dir", CFG_STRING, "results", "Base value for the `${resultdir}` variable, which is used as the default directory for result files (output vector file, output scalar file, eventlog file, etc.). See also the `resultdir-subdivision` config option.");
Register_GlobalConfigOption(CFGID_RESULTDIR_SUBDIVISION, "resultdir-subdivision", CFG_BOOL, "false", "Makes the results directory hierarchical by appending `${iterationvarsd}` to the value of the `result-dir` config option. This is useful if a parameter study produces a large number of runs (>10000), as many file managers and other tools (including the OMNeT++ IDE) struggle with directories containing that many files. An alternative to using this option is to include iteration variables directly in the value of the `result-dir` option.");


// table to be kept consistent with scave/fields.cc
static struct ConfigVarDescription { const char *name, *description; } configVarDescriptions[] = {
    { CFGVAR_RUNID,            "A reasonably globally unique identifier for the run, produced by concatenating the configuration name, run number, date/time, etc." },
    { CFGVAR_INIFILE,          "Name of the (primary) inifile" },
    { CFGVAR_CONFIGNAME,       "Name of the active configuration" },
    { CFGVAR_RUNNUMBER,        "Sequence number of the current run within all runs in the active configuration" },
    { CFGVAR_DESCRIPTION,      "Description of the current configuration" },
    { CFGVAR_NETWORK,          "Value of the `network` configuration option" },
    { CFGVAR_EXPERIMENT,       "Value of the `experiment-label` configuration option" },
    { CFGVAR_MEASUREMENT,      "Value of the `measurement-label` configuration option" },
    { CFGVAR_REPLICATION,      "Value of the `replication-label` configuration option" },
    { CFGVAR_PROCESSID,        "PID of the simulation process" },
    { CFGVAR_DATETIME,         "Date and time the simulation run was started" },
    { CFGVAR_DATETIMEF,        "Like ${datetime}, but sanitized for use as part of a file name" },
    { CFGVAR_RESULTDIR,        "Value of the `result-dir` configuration option" },
    { CFGVAR_REPETITION,       "The iteration number in `0..N-1`, where `N` is the value of the `repeat` configuration option" },
    { CFGVAR_SEEDSET,          "Value of the `seed-set` configuration option" },
    { CFGVAR_ITERATIONVARS,    "Concatenation of all user-defined iteration variables in `name=value` form" },
    { CFGVAR_ITERATIONVARSF,   "Like ${iterationvars}, but sanitized for use as part of a file name" },
    { CFGVAR_ITERATIONVARSD,   "Like ${iterationvars}, but for use as hierarchical folder name (it contains slashes where ${iterationvarsf} has commas)" },
    { nullptr,                 nullptr }
};

class ENVIR_API InifileContentsBuilder : public cConfigurationReader::Callback
{
  private:
    InifileContents *ini;
    int currentSectionIndex = -1;
  public:
    InifileContentsBuilder(InifileContents *target) : ini(target) {}
    virtual ~InifileContentsBuilder() {}
    virtual void sectionHeader(const char *sectionName, const FileLine& fileLine) override;
    virtual void keyValue(const char *key, const char *value, const char *comment, const char *baseDir, const FileLine& fileLine) override;
};

void InifileContentsBuilder::sectionHeader(const char *sectionName, const FileLine& fileLine)
{
    currentSectionIndex = ini->findOrAddSection(sectionName);
}

void InifileContentsBuilder::keyValue(const char *key, const char *value, const char *comment, const char *baseDir, const FileLine& fileLine)
{
    if (currentSectionIndex == -1)
        currentSectionIndex = ini->findOrAddSection("General");
    ini->addEntry(currentSectionIndex, InifileContents::Entry(baseDir, key, value, comment, ini->getSectionName(currentSectionIndex), fileLine));
}

//----------------

InifileContents::~InifileContents()
{
}

void InifileContents::readUsing(cConfigurationReader *reader, cConfiguration *readerConfig)
{
    //TODO:rootFilename = ...
    //TODO:defaultBasedir = ...

    InifileContentsBuilder builder(this);
    reader->setCallback(&builder);
    reader->read(readerConfig);
}

void InifileContents::readFile(const char *filename)
{
    rootFilename = filename;
    defaultBasedir = directoryOf(filename);

    InifileContentsBuilder builder(this);
    InifileReader reader;
    reader.setCallback(&builder);
    reader.readFile(filename);
}

void InifileContents::readText(const char *text, const char *filename="<string>", const char *baseDir=nullptr)
{
    rootFilename = filename;
    defaultBasedir = baseDir;

    InifileReader reader;
    InifileContentsBuilder builder(this);
    reader.setCallback(&builder);
    reader.readText(text, filename, baseDir);
}

void InifileContents::readStream(std::istream& in, const char *filename="<string>", const char *baseDir=nullptr)
{
    rootFilename = filename;
    defaultBasedir = baseDir;

    InifileReader reader;
    InifileContentsBuilder builder(this);
    reader.setCallback(&builder);
    reader.readStream(in, filename, baseDir);
}

const char *InifileContents::getFileName() const
{
    return rootFilename.c_str();
}

const char *InifileContents::getDefaultBaseDirectory() const
{
    return defaultBasedir.c_str();
}

int InifileContents::getNumSections() const
{
    return sections.size();
}

const char *InifileContents::getSectionName(int sectionId) const
{
    const Section& section = getSection(sectionId);
    return section.name.c_str();
}

int InifileContents::getNumEntries(int sectionId) const
{
    const Section& section = getSection(sectionId);
    return section.entries.size();
}

void InifileContents::checkSectionIndex(int sectionId) const
{
    if (sectionId < 0 || sectionId >= (int)sections.size())
        throw cRuntimeError("Section index %d out of bounds", sectionId);
}

void InifileContents::checkEntryIndex(const Section& section, int entryId) const
{
    if (entryId < 0 || entryId >= (int)section.entries.size())
        throw cRuntimeError("Entry index %d out of bounds", entryId);
}

const InifileContents::Entry& InifileContents::getEntry(int sectionId, int entryId) const
{
    checkSectionIndex(sectionId);
    const Section& section = getSection(sectionId);
    checkEntryIndex(section, entryId);
    return section.entries[entryId];
}

const InifileContents::Section& InifileContents::getSection(int sectionId) const
{
    checkSectionIndex(sectionId);
    return sections[sectionId];
}

int InifileContents::findSection(const char *section) const
{
   // not very efficient (linear search), but we only invoke it a few times during activateConfig()
   for (int i = 0; i < getNumSections(); i++)
       if (strcmp(section, getSectionName(i)) == 0)
           return i;
   return -1;
}

int InifileContents::addSection(const char *sectionName)
{
    if (findSection(sectionName) != -1)
        throw cRuntimeError("Attempt to add duplicate section %s", sectionName);
    invalidateCaches();
    Section section;
    section.name = sectionName;
    sections.push_back(section);
    return sections.size()-1;
}

int InifileContents::findOrAddSection(const char *sectionName)
{
    int sectionId = findSection(sectionName);
    return sectionId != -1 ? sectionId : addSection(sectionName);
}

void InifileContents::insertSection(int atSectionId, const char *sectionName)
{
    if (findSection(sectionName) != -1)
        throw cRuntimeError("Attempt to add duplicate section %s", sectionName);
    if (atSectionId != sections.size())  // index=size also accepted
        checkSectionIndex(atSectionId);
    Section section;
    section.name = sectionName;
    sections.insert(sections.begin()+atSectionId, section);
    invalidateCaches();
}

int InifileContents::findEntry(int sectionId, const char *key) const
{
    checkSectionIndex(sectionId);
    const Section& section = sections.at(sectionId);
    int n = section.entries.size();
    for (int i = 0; i < n; i++)
        if (strcmp(key, section.entries[i].getKey()) == 0)
            return i;
    return -1;
}

int InifileContents::addEntry(int sectionId, const Entry& entry)
{
    invalidateCaches();
    checkSectionIndex(sectionId);
    Section& section = sections.at(sectionId);
    section.entries.push_back(entry);
    return section.entries.size()-1;
}

void InifileContents::insertEntry(int sectionId, int atEntryId, const Entry& entry)
{
    checkSectionIndex(sectionId);
    Section& section = sections.at(sectionId);
    if (atEntryId != section.entries.size())
        checkEntryIndex(section, atEntryId);
    section.entries.insert(section.entries.begin()+atEntryId, entry);
    invalidateCaches();
}

void InifileContents::deleteSection(int sectionId)
{
    checkSectionIndex(sectionId);
    sections.erase(sections.begin()+sectionId);
    invalidateCaches();
}

void InifileContents::deleteEntry(int sectionId, int entryId)
{
    checkSectionIndex(sectionId);
    Section& section = sections.at(sectionId);
    checkEntryIndex(section, entryId);
    section.entries.erase(section.entries.begin()+entryId);
    invalidateCaches();
}

void InifileContents::setCommandLineConfigOptions(const std::map<std::string, std::string>& options, const char *baseDir)
{
   commandLineOptions.clear();
   invalidateCaches();
   for (const auto & it : options) {
       const char *key = it.first.c_str();
       const char *value = it.second.c_str();
       if (opp_isempty(value))
           throw cRuntimeError("Missing value for command-line option --%s", key);
       commandLineOptions.push_back(Entry(baseDir, key, value, "", "", FileLine("<command-line>")));
   }
}

void InifileContents::invalidateCaches()
{
    cachedSectionChains.clear();
}

void InifileContents::clear()
{
    rootFilename = "";
    defaultBasedir = "";
    sections.clear();
    commandLineOptions.clear();
    cachedSectionChains.clear();
}

std::vector<std::string> InifileContents::getConfigNames()
{
   std::vector<std::string> result;
   for (int i = 0; i < getNumSections(); i++)
       result.push_back(getSectionName(i));
   return result;
}

std::string InifileContents::getConfigDescription(const char *configName) const
{
   // determine the list of sections, from this one up to [General]
   std::vector<int> sectionChain = resolveSectionChain(configName);
   return internalGetValueAsString(sectionChain, CFGID_DESCRIPTION->getName());
}

std::vector<std::string> InifileContents::getBaseConfigs(const char *configName) const
{
   int sectionId = resolveConfigName(configName);
   if (sectionId == -1)
       throw cRuntimeError("No such config: %s", configName);
   int entryId = findEntry(sectionId, CFGID_EXTENDS->getName());
   std::string extends = entryId == -1 ? "" : getEntry(sectionId, entryId).getValue();
   if (extends.empty())
       extends = "General";

   std::vector<std::string> result;
   StringTokenizer tokenizer(extends.c_str(), ", \t");
   while (tokenizer.hasMoreTokens()) {
       const char *sectionName = tokenizer.nextToken();
       int sectionId = resolveConfigName(sectionName);
       if (sectionId != -1)
           result.push_back(sectionName);
   }
   return result;
}

std::vector<int> InifileContents::getBaseConfigIds(int sectionId) const
{
   int generalSectionId = findSection("General");
   int entryId = findEntry(sectionId, CFGID_EXTENDS->getName());
   std::string extends = entryId == -1 ? "" : getEntry(sectionId, entryId).getValue();

   std::vector<int> result;
   if (sectionId == generalSectionId && generalSectionId != -1) {
       // 'General' can not have base config
   }
   else if (extends.empty()) {
       // implicitly inherit form 'General'
       if (generalSectionId != -1)
           result.push_back(generalSectionId);
   }
   else {
       StringTokenizer tokenizer(extends.c_str(), ", \t");
       while (tokenizer.hasMoreTokens()) {
           const char *configName = tokenizer.nextToken();
           int sectionId = resolveConfigName(configName);
           if (sectionId != -1)
               result.push_back(sectionId);
           else
               throw cRuntimeError("Section not found: %s", configName);
       }
   }
   return result;
}

std::vector<std::string> InifileContents::getConfigChain(const char *configName) const
{
   std::vector<std::string> result;
   std::vector<int> sectionIds = resolveSectionChain(configName);
   for (int sectionId : sectionIds)
       result.push_back(getSectionName(sectionId));
   return result;
}

int InifileContents::resolveConfigName(const char *configName) const
{
   if (!configName || !configName[0])
       throw cRuntimeError("Empty config name specified");
   return findSection(configName);
}

bool InifileContents::isGlobalOption(const char *key)
{
    if (strchr(key, '.') != nullptr)
        return false;
    cConfigOption *option = lookupConfigOption(key);
    return option != nullptr && !option->isPerObject();
}

cConfiguration *InifileContents::extractGlobalConfig()
{
    // extract the global options only
    std::vector<Entry> entries;
    for (auto & e : commandLineOptions)
        if (isGlobalOption(e.getKey()))
            entries.push_back(e); // note: no substituteVariables() on value, it's too early for that

    int sectionId = resolveConfigName("General");
    if (sectionId != -1) {
        for (int entryId = 0; entryId < getNumEntries(sectionId); entryId++) {
            const auto& e = getEntry(sectionId, entryId);
            if (isGlobalOption(e.getKey()))
                entries.push_back(e); // note: no substituteVariables() on value, it's too early for that
        }
    }

//   VariablesInfo variables = computeVariables(configName, runNumber, sectionChain, &scenario, locationToVarName); //TODO

    const StringMap predefinedVars = StringMap(); //TODO
    const StringMap iterationVars;  // empty
    return new Configuration(entries, predefinedVars, iterationVars, rootFilename.c_str());
}

cConfiguration *InifileContents::extractConfig(const char *configName, int runNumber)
{
   // determine the list of sections, from this one up to [General]
   std::vector<int> sectionChain = resolveSectionChain(configName);

   // extract all iteration vars from values within this section
   StringMap locationToVarName;
   std::vector<Scenario::IterationVariable> itervars = collectIterationVariables(sectionChain, locationToVarName); // also fills locationToVarName

   // see if there's a constraint and/or iteration nesting order given
   const char *constraint = internalGetValue(sectionChain, CFGID_CONSTRAINT->getName(), nullptr);
   const char *nestingSpec = internalGetValue(sectionChain, CFGID_ITERATION_NESTING_ORDER->getName(), nullptr);

   VariablesInfo variables;

   // determine the values to substitute into the iteration vars (${...})
   try {
       Scenario scenario(itervars, constraint, nestingSpec);
       int numRuns = scenario.getNumRuns();
       if (runNumber < 0 || runNumber >= numRuns)
           throw cRuntimeError("Run number %d is out of range for configuration '%s': It contains %d run(s)", runNumber, configName, numRuns);

       scenario.gotoRun(runNumber);
       variables = computeVariables(configName, runNumber, sectionChain, &scenario, locationToVarName);
   }
   catch (std::exception& e) {
       throw cRuntimeError("Scenario generator: %s", e.what());
   }

   auto addWildcardIfNeeded = [](const char *key) {
       if (strchr(key, '.') == nullptr) {
           cConfigOption *e = lookupConfigOption(key);
           if (e && e->isPerObject())
               return std::string("**.") + key; // if it's per-object option but specified without an object path, pretend it was specified with "**."
       }
       return std::string(key);
   };

   std::vector<Entry> entries;

   // concatenate the contents of the sections and the command line into a common flat list (entries[])
   for (auto & e : commandLineOptions) {
       std::string key = addWildcardIfNeeded(e.getKey());
       std::string value = substituteVariables(e.getValue(), variables, -1, -1);
       entries.push_back(Entry(e.getBaseDirectory(), key.c_str(), value.c_str(), e.getComment(), e.getOriginSection(), e.getSourceLocation()));
   }
   for (int sectionId : sectionChain) {
       for (int entryId = 0; entryId < getNumEntries(sectionId); entryId++) {
           // add entry to our tables
           const auto& e = getEntry(sectionId, entryId);
           std::string key = addWildcardIfNeeded(e.getKey());
           std::string value = substituteVariables(e.getValue(), variables, sectionId, entryId);
           entries.push_back(Entry(e.getBaseDirectory(), key.c_str(), value.c_str(), e.getComment(), e.getOriginSection(), e.getSourceLocation()));
       }
   }

   return new Configuration(entries, variables.predefinedVariables, variables.iterationVariables, rootFilename.c_str());
}

inline std::string unquote(const std::string& txt)
{
   if (txt.find('"') == std::string::npos)
       return txt;
   try {
       return opp_parsequotedstr(txt.c_str());
   }
   catch (std::exception& e) {
       return txt;
   }
}

InifileContents::VariablesInfo InifileContents::computeVariables(const char *configName, int runNumber, std::vector<int> sectionChain, const Scenario *scenario, const StringMap& locationToVarName) const
{
    VariablesInfo result;

    result.locationToVarName = locationToVarName;

    // collect iteration variables
    std::vector<std::string> iterVarNames = scenario->getIterationVariableNames();
    remove(iterVarNames, std::string(CFGVAR_REPETITION)); // not really an itervar
    for (std::string varName : iterVarNames)
        result.iterationVariables[varName] = scenario->getVariable(varName.c_str());

    // assemble ${iterationvars}, ${iterationvarsf} and ${iterationvarsd}
   std::string iterationvars, iterationvarsf, iterationvarsd;
   for (const std::string& varName : iterVarNames) {
           std::string value = result.iterationVariables[varName];
           iterationvars += std::string(iterationvars.empty() ? "" : ", ") + "$" + varName + "=" + value;
           std::string segment = (opp_isalpha(varName[0]) ? varName + "=" : "") + opp_filenameencode(unquote(value)); // without dollar and space, possible quotes removed
           iterationvarsf += std::string(iterationvarsf.empty() ? "" : ",") + segment;
           iterationvarsd += std::string(iterationvarsd.empty() ? "" : "/") + segment;
   }
   if (!iterationvarsf.empty())
       iterationvarsf += "-";  // for filenames
   result.predefinedVariables[CFGVAR_ITERATIONVARS] = iterationvars;
   result.predefinedVariables[CFGVAR_ITERATIONVARSF] = iterationvarsf;
   result.predefinedVariables[CFGVAR_ITERATIONVARSD] = iterationvarsd;

   // create variables
   int runnumberWidth = internalGetConfigAsInt(CFGID_RUNNUMBER_WIDTH, sectionChain, result);
   runnumberWidth = std::max(0, std::min(6, runnumberWidth));
   std::string datetime = opp_makedatetimestring();
   result.predefinedVariables[CFGVAR_REPETITION] = scenario->getVariable(CFGVAR_REPETITION);
   if (!opp_isempty(getFileName()))
       result.predefinedVariables[CFGVAR_INIFILE] = getFileName();
   result.predefinedVariables[CFGVAR_CONFIGNAME] = configName;
   result.predefinedVariables[CFGVAR_RUNNUMBER] = opp_stringf("%0*d", runnumberWidth, runNumber);
   std::string description = internalGetConfigAsString(CFGID_DESCRIPTION, sectionChain, result);
   if (!description.empty())
       result.predefinedVariables[CFGVAR_DESCRIPTION] = description;
   result.predefinedVariables[CFGVAR_NETWORK] = internalGetConfigAsString(CFGID_NETWORK, sectionChain, result);
   result.predefinedVariables[CFGVAR_PROCESSID] = opp_stringf("%d", (int)getpid());
   result.predefinedVariables[CFGVAR_DATETIME] = datetime;
   result.predefinedVariables[CFGVAR_DATETIMEF] = opp_replacesubstring(datetime, ":", "", true);
   result.predefinedVariables[CFGVAR_RUNID] = result.predefinedVariables[CFGVAR_CONFIGNAME]+"-"+result.predefinedVariables[CFGVAR_RUNNUMBER]+"-"+result.predefinedVariables[CFGVAR_DATETIME]+"-"+result.predefinedVariables[CFGVAR_PROCESSID];

   // the following variables should be done last, because they may depend on the variables computed above
   std::string resultdir = internalGetConfigAsString(CFGID_RESULT_DIR, sectionChain, result);
   bool subdivideResultdir = internalGetConfigAsBool(CFGID_RESULTDIR_SUBDIVISION, sectionChain, result);
   if (subdivideResultdir)
       resultdir += std::string("/") + configName + "/" + iterationvarsd;
   result.predefinedVariables[CFGVAR_RESULTDIR] = resultdir;
   result.predefinedVariables[CFGVAR_SEEDSET] = std::to_string(internalGetConfigAsInt(CFGID_SEED_SET, sectionChain, result));
   result.predefinedVariables[CFGVAR_EXPERIMENT] = internalGetConfigAsString(CFGID_EXPERIMENT_LABEL, sectionChain, result);
   result.predefinedVariables[CFGVAR_MEASUREMENT] = internalGetConfigAsString(CFGID_MEASUREMENT_LABEL, sectionChain, result);
   result.predefinedVariables[CFGVAR_REPLICATION] = internalGetConfigAsString(CFGID_REPLICATION_LABEL, sectionChain, result);

   return result;
}

std::string InifileContents::internalGetConfigAsString(cConfigOption *option, const std::vector<int>& sectionChain, const VariablesInfo& variables) const
{
   try {
       int sectionId, entryId;
       const char *value = internalGetValue(sectionChain, option->getName(), option->getDefaultValue(), &sectionId, &entryId);
       std::string str = substituteVariables(value, variables, sectionId, entryId);
       if (str[0] == '"')
           str = Expression().parse(str.c_str()).stringValue();
       return str;
   }
   catch (std::exception& e) {
       throw cRuntimeError("Error getting value of config option '%s': %s", option->getName(), e.what());
   }
}

intval_t InifileContents::internalGetConfigAsInt(cConfigOption *option, const std::vector<int>& sectionChain, const VariablesInfo& variables) const
{
   try {
       int sectionId, entryId;
       const char *value = internalGetValue(sectionChain, option->getName(), option->getDefaultValue(), &sectionId, &entryId);
       std::string str = substituteVariables(value, variables, sectionId, entryId);
       return Expression().parse(str.c_str()).intValue();
   }
   catch (std::exception& e) {
       throw cRuntimeError("Error getting value of config option '%s': %s", option->getName(), e.what());
   }
}

bool InifileContents::internalGetConfigAsBool(cConfigOption *option, const std::vector<int>& sectionChain, const VariablesInfo& variables) const
{
   try {
       int sectionId, entryId;
       const char *value = internalGetValue(sectionChain, option->getName(), option->getDefaultValue(), &sectionId, &entryId);
       std::string str = substituteVariables(value, variables, sectionId, entryId);
       return Expression().parse(str.c_str()).boolValue();
   }
   catch (std::exception& e) {
       throw cRuntimeError("Error getting value of config option '%s': %s", option->getName(), e.what());
   }
}

int InifileContents::getNumRunsInConfig(const char *configName) const
{
   // extract all iteration vars from values within this config
   std::vector<int> sectionChain = resolveSectionChain(configName);
   StringMap locationToVarNameMap;
   std::vector<Scenario::IterationVariable> v = collectIterationVariables(sectionChain, locationToVarNameMap); // also fills locationToVarNameMap

   // see if there's a constraint given
   const char *constraint = internalGetValue(sectionChain, CFGID_CONSTRAINT->getName(), nullptr);

   // count the runs and return the result
   try {
       return Scenario(v, constraint, "").getNumRuns();
   }
   catch (std::exception& e) {
       throw cRuntimeError("Could not compute number of runs in config %s: %s", configName, e.what());
   }
}

std::vector<InifileContents::RunInfo> InifileContents::unrollConfig(const char *configName) const
{
   // extract all iteration vars from values within this section
   std::vector<int> sectionChain = resolveSectionChain(configName);
   StringMap locationToVarNameMap;
   std::vector<Scenario::IterationVariable> itervars = collectIterationVariables(sectionChain, locationToVarNameMap);

   // see if there's a constraint and/or iteration nesting order given
   const char *constraint = internalGetValue(sectionChain, CFGID_CONSTRAINT->getName(), nullptr);
   const char *nestingSpec = internalGetValue(sectionChain, CFGID_ITERATION_NESTING_ORDER->getName(), nullptr);

   // iterate over all runs in the scenario
   try {
       Scenario scenario(itervars, constraint, nestingSpec);
       std::vector<RunInfo> result;
       if (scenario.restart()) {
           for (;;) {
               int runNumber = result.size();
               VariablesInfo variables = computeVariables(configName, runNumber, sectionChain, &scenario, locationToVarNameMap);

               RunInfo runInfo;
               runInfo.info = scenario.str();
               runInfo.iterVars = variables.iterationVariables;
               runInfo.runAttrs = variables.predefinedVariables;

               // collect entries that contain ${..}
               std::string tmp;
               for (int sectionId : sectionChain) {
                   for (int entryId = 0; entryId < getNumEntries(sectionId); entryId++) {
                       const auto& entry = getEntry(sectionId, entryId);
                       if (strstr(entry.getValue(), "${") != nullptr) {
                           std::string expandedValue = substituteVariables(entry.getValue(), variables, sectionId, entryId);
                           tmp += std::string(entry.getKey()) + " = " + expandedValue + "\n";
                       }
                   }
               }
               runInfo.configBrief = tmp;
               result.push_back(runInfo);

               // move to the next run
               if (!scenario.next())
                   break;
           }
       }
       return result;
   }
   catch (std::exception& e) {
       throw cRuntimeError("Scenario generator: %s", e.what());
   }
}

std::vector<int> InifileContents::resolveRunFilter(const char *configName, const char *runFilter)
{
    std::vector<int> runNumbers;

    if (opp_isblank(runFilter)) {
        int numRuns = getNumRunsInConfig(configName);
        for (int i = 0; i < numRuns; i++)
            runNumbers.push_back(i);
        return runNumbers;
    }

    // if filter contains a list of run numbers (e.g. "0..4,9,12"), parse it accordingly
    if (strspn (runFilter, "0123456789,.- ") == strlen(runFilter)) {
        int numRuns = getNumRunsInConfig(configName);
        EnumStringIterator it(runFilter);
        while (true) {
            int runNumber = it();
            if (runNumber == -1)
                break;
            if (runNumber >= numRuns)
                throw cRuntimeError("Run number %d in run list '%s' is out of range 0..%d", runNumber, runFilter, numRuns-1);
            runNumbers.push_back(runNumber);
            it++;
        }
        if (it.hasError())
            throw cRuntimeError("Syntax error in run list '%s'", runFilter);
    }
    else {
        // evaluate filter as constraint expression
        std::vector<InifileContents::RunInfo> runDescriptions = unrollConfig(configName);
        for (int runNumber = 0; runNumber < (int) runDescriptions.size(); runNumber++) {
            try {
                InifileContents::RunInfo runInfo = runDescriptions[runNumber];
                std::string expandedRunFilter = opp_substitutevariables(runFilter, unionOf(runInfo.runAttrs, runInfo.iterVars));
                ValueIterator::Expr expr(expandedRunFilter.c_str());
                expr.substituteVariables(ValueIterator::VariableMap());
                expr.evaluate();
                if (expr.boolValue())
                    runNumbers.push_back(runNumber);
            }
            catch (std::exception& e) {
                throw cRuntimeError("Cannot evaluate run filter: %s", e.what());
            }
        }
    }
    return runNumbers;
}

std::vector<Scenario::IterationVariable> InifileContents::collectIterationVariables(const std::vector<int>& sectionChain, StringMap& outLocationToNameMap) const
{
   std::vector<Scenario::IterationVariable> v;
   int unnamedCount = 0;
   outLocationToNameMap.clear();
   for (int sectionId : sectionChain) {
       for (int entryId = 0; entryId < getNumEntries(sectionId); entryId++) {
           const auto& entry = getEntry(sectionId, entryId);
           const char *pos = entry.getValue();
           int k = 0;
           while ((pos = strstr(pos, "${")) != nullptr) {
               Scenario::IterationVariable iterVar;
               try {
                   parseVariable(pos, iterVar.varName, iterVar.value, iterVar.parvar, pos);
               }
               catch (std::exception& e) {
                   throw cRuntimeError("Scenario generator: %s at %s=%s", e.what(), entry.getKey(), entry.getValue());
               }
               if (!iterVar.value.empty()) {
                   // store variable
                   if (!iterVar.varName.empty()) {
                       // check it does not conflict with other iteration variables or predefined variables
                       for (auto & j : v)
                           if (j.varName == iterVar.varName)
                               throw cRuntimeError("Scenario generator: Redefinition of iteration variable ${%s} in the configuration", iterVar.varName.c_str());

                       if (isPredefinedVariable(iterVar.varName.c_str()))
                           throw cRuntimeError("Scenario generator: ${%s} is a predefined variable and cannot be changed", iterVar.varName.c_str());
                   }
                   else {
                       // unnamed variable: generate name ($0, $1, $2, etc.), and store its location
                       iterVar.varName = opp_stringf("%d", unnamedCount++);
                       std::string location = opp_stringf("%d:%d:%d", sectionId, entryId, k);
                       outLocationToNameMap[location] = iterVar.varName;
                   }
                   v.push_back(iterVar);
               }
               k++;
           }
       }
   }

   // register ${repetition}, based on the repeat= config entry
   int repeatCount = internalGetValueAsInt(sectionChain, CFGID_REPEAT->getName(), 1);
   Scenario::IterationVariable repetition;
   repetition.varName = CFGVAR_REPETITION;
   repetition.value = opp_stringf("0..%d", repeatCount-1);
   v.push_back(repetition);

   return v;
}

void InifileContents::parseVariable(const char *txt, std::string& outVarname, std::string& outValue, std::string& outParvar, const char *& outEndPtr)
{
    Assert(txt[0] == '$' && txt[1] == '{');  // this is the way we've got to be invoked

    StringTokenizer tokenizer(txt+2, "}", StringTokenizer::NO_TRIM | StringTokenizer::HONOR_QUOTES | StringTokenizer::HONOR_PARENS); // NO_TRIM is important because do strlen(token) to find the "}"!
    const char *token = tokenizer.nextToken();  // ends at matching '}'
    if (!token)
        throw cRuntimeError("Missing '}' for '${'");
    outEndPtr = txt + 2 + strlen(token) + 1;
    Assert(*(outEndPtr-1) == '}');

    // parse what's inside the ${...}
    const char *varNameBegin = nullptr;
    const char *varNameEnd = nullptr;
    const char *valueBegin = nullptr;
    const char *valueEnd = nullptr;
    const char *parvarBegin = nullptr;
    const char *parvarEnd = nullptr;

    const char *s = txt+2;
    while (opp_isspace(*s))
        s++;
    if (opp_isalphaext(*s)) {
        // must be a variable or a variable reference
        varNameBegin = varNameEnd = s;
        while (opp_isalnumext(*varNameEnd))
            varNameEnd++;
        s = varNameEnd;
        while (opp_isspace(*s))
            s++;
        if (*s == '}') {
            // ${x} syntax -- OK
        }
        else if (*s == '=' && *(s+1) != '=') {
            // ${x=...} syntax -- OK
            valueBegin = s+1;
        }
        else if (strchr(s, ',')) {
            // part of a valuelist -- OK
            valueBegin = varNameBegin;
            varNameBegin = varNameEnd = nullptr;
        }
        else {
            throw cRuntimeError("Missing '=' after '${varname'");
        }
    }
    else {
        valueBegin = s;
    }
    valueEnd = outEndPtr-1;

    if (valueBegin) {
        // try to parse parvar, present when value ends in "! variable"
        const char *exclamationMark = strrchr(valueBegin, '!');
        if (exclamationMark) {
            const char *s = exclamationMark+1;
            while (opp_isspace(*s))
                s++;
            if (opp_isalphaext(*s)) {
                parvarBegin = s;
                while (opp_isalnumext(*s))
                    s++;
                parvarEnd = s;
                while (opp_isspace(*s))
                    s++;
                if (s != valueEnd) {
                    parvarBegin = parvarEnd = nullptr;  // no parvar after all
                }
            }
            if (parvarBegin) {
                valueEnd = exclamationMark;  // chop off "!parvarname"
            }
        }
    }

    outVarname = outValue = outParvar = "";
    if (varNameBegin)
        outVarname.assign(varNameBegin, varNameEnd-varNameBegin);
    if (valueBegin)
        outValue.assign(valueBegin, valueEnd-valueBegin);
    if (parvarBegin)
        outParvar.assign(parvarBegin, parvarEnd-parvarBegin);
}

std::string InifileContents::substituteVariables(const char *text, const VariablesInfo& variables, int sectionId, int entryId) const
{
    //TODO use opp_substituteVariables()
    std::string result = opp_nulltoempty(text);
    int k = 0;  // counts "${" occurrences
    size_t pos = 0;
    while ((pos = result.find("${", pos)) != std::string::npos) {
        std::string varName, dummy1, dummy2;
        const char *endPtr;
        parseVariable(result.c_str() + pos, varName, dummy1, dummy2, endPtr);
        size_t len = endPtr - (result.c_str() + pos);

        // handle named and unnamed iteration variable references
        if (varName.empty()) {
            std::string location = opp_stringf("%d:%d:%d", sectionId, entryId, k);
            StringMap::const_iterator it = variables.locationToVarName.find(location);
            Assert(it != variables.locationToVarName.end());
            varName = it->second;
        }
        StringMap::const_iterator it = variables.iterationVariables.find(varName);
        if (it == variables.iterationVariables.end()) {
            it = variables.predefinedVariables.find(varName);
            if (it == variables.predefinedVariables.end())
                throw cRuntimeError("No such variable: ${%s}", varName.c_str());
        }
        std::string value = it->second;

        result.replace(pos, len, value);
        pos += value.length(); // skip over contents just inserted

        k++;
    }
    return result;
}

std::vector<std::string> InifileContents::getPredefinedVariableNames() const
{
    std::vector<std::string> result;
    for (int i = 0; configVarDescriptions[i].name; i++)
        result.push_back(configVarDescriptions[i].name);
    return result;
}

bool InifileContents::isPredefinedVariable(const char *varname)
{
     for (int i = 0; configVarDescriptions[i].name; i++)
         if (strcmp(varname, configVarDescriptions[i].name) == 0)
            return true;
    return false;
}

const char *InifileContents::getVariableDescription(const char *varname) const
{
    for (int i = 0; configVarDescriptions[i].name; i++)
        if (strcmp(varname, configVarDescriptions[i].name) == 0)
            return configVarDescriptions[i].description;
    return nullptr;
}

typedef std::vector<int> SectionChain;
typedef std::vector<SectionChain> SectionChainList;

std::vector<int> InifileContents::resolveSectionChain(int sectionId) const
{
   if (sectionId == -1)
       return std::vector<int>();  // assume implicit [General] section
   if (cachedSectionChains.empty())
       cachedSectionChains.resize(getNumSections());
   if (cachedSectionChains.at(sectionId).empty())
       cachedSectionChains[sectionId] = computeSectionChain(sectionId);
   return cachedSectionChains[sectionId];
}

std::vector<int> InifileContents::resolveSectionChain(const char *configName) const
{
   int sectionId = resolveConfigName(configName);
   if (sectionId == -1 && strcmp(configName, "General") != 0)  // allow implicit [General] section
       throw cRuntimeError("No such config: %s", configName);
   return resolveSectionChain(sectionId);
}

static bool mergeSectionChains(SectionChainList& remainingInputs, std::vector<int>& result);

/*
* Computes the linearization of given section and all of its base sections.
* The result is the merge of the linearization of base classes and base classes itself.
*/
std::vector<int> InifileContents::computeSectionChain(int sectionId) const
{
   std::vector<int> baseConfigs = getBaseConfigIds(sectionId);
   SectionChainList chainsToBeMerged;
   for (std::vector<int>::const_iterator it = baseConfigs.begin(); it != baseConfigs.end(); ++it) {
       SectionChain chain = resolveSectionChain(*it);
       chainsToBeMerged.push_back(chain);
   }
   chainsToBeMerged.push_back(baseConfigs);

   std::vector<int> result;
   result.push_back(sectionId);
   if (mergeSectionChains(chainsToBeMerged, result))
       return result;
   else
       throw cRuntimeError("Detected section fallback chain inconsistency at [%s]", getSectionName(sectionId));  // TODO more details?
}

static int selectNext(const SectionChainList& remainingInputs);

/*
* Merges the the section chains given as 'sectionChains' and collects the result in 'result'.
*
* The logic of selecting the next element for the output is implemented in the selectNext() function.
* The Dylan and C3 linearization differs only in the selectNext() function.
*
* The 'sectionChains' is modified during the merge. If the was successful then
* the function returns 'true' and all chain within 'sectionChains' is empty,
* otherwise it returns false and the 'sectionChains' contains conflicting chains.
*/
static bool mergeSectionChains(SectionChainList& sectionChains, std::vector<int>& result)
{
   SectionChainList::iterator begin = sectionChains.begin();
   SectionChainList::iterator end = sectionChains.end();

   // reverse the chains, so they are accessed at the end instead of the begin (more efficient to remove from the end of a vector)
   bool allChainsAreEmpty = true;
   for (SectionChainList::iterator chain = begin; chain != end; ++chain) {
       if (!chain->empty()) {
           reverse(chain->begin(), chain->end());
           allChainsAreEmpty = false;
       }
   }

   while (!allChainsAreEmpty) {
       // select next
       int nextId = selectNext(sectionChains);
       if (nextId == -1)
           break;
       // add it to the output and remove from the inputs
       result.push_back(nextId);
       allChainsAreEmpty = true;
       for (SectionChainList::iterator chain = begin; chain != end; ++chain) {
           if (!chain->empty()) {
               if (chain->back() == nextId)
                   chain->pop_back();
               if (!chain->empty())
                   allChainsAreEmpty = false;
           }
       }
   }

   return allChainsAreEmpty;
}

/*
* Select next element for the merge of 'sectionChains'.
*
* This implements C3 linearization:
*   When there are more that one possible candidates,
*   choose the section that appears in the linearization of the earliest
*   direct base section in the local precedence order (i.e. as enumerated in the 'extends' entry).
*/
static int selectNext(const SectionChainList& sectionChains)
{
   SectionChainList::const_iterator begin = sectionChains.begin();
   SectionChainList::const_iterator end = sectionChains.end();

   for (SectionChainList::const_iterator chain = begin; chain != end; ++chain) {
       if (!chain->empty()) {
           int candidate = chain->back();

           // check if candidate is not contained any tail of input chains
           // note: the head of the chains are at their back()
           bool found = false;
           for (SectionChainList::const_iterator chainPtr = begin; !found && chainPtr != end; ++chainPtr)
               if (!chainPtr->empty())
                   for (SectionChain::const_reverse_iterator idPtr = chainPtr->rbegin()+1; !found && idPtr != chainPtr->rend(); ++idPtr)
                       if (*idPtr == candidate)
                           found = true;

           if (!found)
               return candidate;
       }
   }

   return -1;
}

//TODO eliminate duplication with the Configuration class
void InifileContents::splitKey(const char *key, std::string& outOwnerName, std::string& outBinName)
{
    std::string tmp = key;

    const char *lastDotPos = strrchr(key, '.');
    const char *doubleAsterisk = !lastDotPos ? nullptr : strstr(lastDotPos, "**");

    if (!lastDotPos || doubleAsterisk) {
        // complicated special case: there's a "**" after the last dot
        // (or there's no dot at all). Examples: "**baz", "net.**.foo**",
        // "net.**.foo**bar**baz"
        // Problem with this: are "foo" and "bar" part of the paramname (=bin)
        // or the module name (=owner)? Can be either way. Only feasible solution
        // is to force matching of the full path (modulepath.paramname) against
        // the full pattern. Bin name can be "*" plus segment of the pattern
        // after the last "**". (For example, for "net.**foo**bar", the bin name
        // will be "*bar".)

        // find last "**"
        while (doubleAsterisk && strstr(doubleAsterisk+1, "**"))
            doubleAsterisk = strstr(doubleAsterisk+1, "**");
        outOwnerName = "";  // empty owner means "do fullPath match"
        outBinName = !doubleAsterisk ? "*" : doubleAsterisk+1;
    }
    else {
        // normal case: bin is the part after the last dot
        outOwnerName.assign(key, lastDotPos - key);
        outBinName.assign(lastDotPos+1);
    }
}

const char *InifileContents::internalGetValue(const std::vector<int>& sectionChain, const char *key, const char *fallbackValue, int *outSectionIdPtr, int *outEntryIdPtr) const
{
   if (outSectionIdPtr)
       *outSectionIdPtr = -1;
   if (outEntryIdPtr)
       *outEntryIdPtr = -1;

   for (const auto & commandLineOption : commandLineOptions)
       if (strcmp(key, commandLineOption.getKey()) == 0)
           return commandLineOption.getValue();


   for (int sectionId : sectionChain) {
       int entryId = findEntry(sectionId, key);
       if (entryId != -1) {
           if (outSectionIdPtr)
               *outSectionIdPtr = sectionId;
           if (outEntryIdPtr)
               *outEntryIdPtr = entryId;
           return getEntry(sectionId, entryId).getValue();
       }
   }
   return fallbackValue;
}

std::string InifileContents::internalGetValueAsString(const std::vector<int>& sectionChain, const char *key, const char *fallbackValue, int *outSectionIdPtr, int *outEntryIdPtr) const
{
   const char *s = internalGetValue(sectionChain, key, fallbackValue, outSectionIdPtr, outEntryIdPtr);
   return !s ? "" : s[0]=='"' ? Expression().parse(s).stringValue() : s;
}

intval_t InifileContents::internalGetValueAsInt(const std::vector<int>& sectionChain, const char *key, intval_t fallbackValue, int *outSectionIdPtr, int *outEntryIdPtr) const
{
   const char *s = internalGetValue(sectionChain, key, nullptr, outSectionIdPtr, outEntryIdPtr);
   return !s ? fallbackValue : Expression().parse(s).intValue();
}

enum { WHITE, GREY, BLACK };

class SectionGraphNode
{
 public:
   int id;
   std::vector<int> nextNodes;
   int color;

   SectionGraphNode(int id) : id(id), color(WHITE) {}
};

typedef std::vector<SectionGraphNode> SectionGraph;

static bool visit(SectionGraph& graph, SectionGraphNode& node);

static int findCycle(SectionGraph& graph)
{
   for (SectionGraph::iterator node = graph.begin(); node != graph.end(); ++node)
       if (node->color == WHITE && visit(graph, *node))
           return node->id;
   return -1;
}

static bool visit(SectionGraph& graph, SectionGraphNode& node)
{
   node.color = GREY;
   for (int & nextNode : node.nextNodes) {
       SectionGraphNode& node2 = graph[nextNode];
       if (node2.color == GREY || (node2.color == WHITE && visit(graph, node2)))
           return true;
   }
   node.color = BLACK;
   return false;
}

void InifileContents::validate(const char *ignorableConfigKeys) const
{
   // check command-line options
   for (const auto & entry : commandLineOptions) {
       const char *key = entry.getKey();
       bool containsDot = strchr(key, '.') != nullptr;
       const char *option = strchr(key, '.') ? strrchr(key, '.')+1 : key;  // check only the part after the last dot, i.e. recognize per-object keys as well
       bool lastSegmentContainsHyphen = strchr(option, '-') != nullptr;
       bool isConfigKey = !containsDot || lastSegmentContainsHyphen;
       if (isConfigKey) {
           cConfigOption *e = lookupConfigOption(option);
           if (!e)
               throw cRuntimeError("Unknown command-line configuration option --%s", key);
           if (containsDot && !e->isPerObject())
               throw cRuntimeError("Invalid command-line configuration option --%s: %s is not a per-object option", key, e->getName());
       }
   }

   // check section names
   std::set<std::string> configNames;
   for (int i = 0; i < getNumSections(); i++) {
       const char *configName = getSectionName(i);
       if (configName) {
           if (strchr(configName, ' ') != nullptr)
               throw cRuntimeError("Invalid section name [%s]: It may not contain spaces", configName);
           if (!opp_isalphaext(*configName) && *configName != '_')
               throw cRuntimeError("Invalid section name [%s]: Must begin with a letter or underscore", configName);
           for (const char *s = configName; *s; s++)
               if (!opp_isalnumext(*s) && strchr("-_@", *s) == nullptr)
                   throw cRuntimeError("Invalid section name [%s]: Contains illegal character '%c'", configName, *s);
           if (configNames.find(configName) != configNames.end())
               throw cRuntimeError("Duplicate section [%s]", configName);
           configNames.insert(configName);
       }
   }

   // check keys, build inheritance graph
   SectionGraph graph;
   for (int i = 0; i < getNumSections(); i++) {
       graph.push_back(SectionGraphNode(i));
       const char *section = getSectionName(i);
       int numEntries = getNumEntries(i);
       for (int j = 0; j < numEntries; j++) {
           const char *key = getEntry(i, j).getKey();
           bool containsDot = strchr(key, '.') != nullptr;

           if (!containsDot && !PatternMatcher::containsWildcards(key)) {
               // warn for unrecognized (or misplaced) config keys
               // NOTE: values don't need to be validated here, that will be
               // done when the config gets actually used
               cConfigOption *e = lookupConfigOption(key);
               if (!e && isIgnorableConfigKey(ignorableConfigKeys, key))
                   continue;
               if (!e)
                   throw cRuntimeError("Unknown configuration option: %s", key);
               if (e->isPerObject())
                   throw cRuntimeError("Configuration option %s should be specified per object, try **.%s=", key, key); //TODO this could now be allowed (it works)

               // check section hierarchy
               if (strcmp(key, CFGID_EXTENDS->getName()) == 0) {
                   if (strcmp(section, "General") == 0)
                       throw cRuntimeError("The [General] section cannot extend other sections");

                   // warn for invalid "extends" names
                   const char *value = getEntry(i, j).getValue();
                   StringTokenizer tokenizer(value, ", \t");
                   while (tokenizer.hasMoreTokens()) {
                       const char *configName = tokenizer.nextToken();
                       int configId = findSection(configName);
                       if (strcmp(configName, "General") != 0 && configId == -1)
                           throw cRuntimeError("No such config: %s", configName);
                       if (configId != -1)
                           graph.back().nextNodes.push_back(configId);
                   }
               }
           }
           else {
               // check for per-object configuration subkeys (".ev-enabled", ".record-interval")
               std::string ownerName;
               std::string suffix;
               splitKey(key, ownerName, suffix);
               bool containsHyphen = strchr(suffix.c_str(), '-') != nullptr;
               if (containsHyphen) {
                   // this is a per-object config
                   cConfigOption *e = lookupConfigOption(suffix.c_str());
                   if (!e && isIgnorableConfigKey(ignorableConfigKeys, suffix.c_str()))
                       continue;
                   if (!e || !e->isPerObject()) {
                       throw cRuntimeError("Unknown per-object configuration option '%s' in %s", suffix.c_str(), key);
                   }
               }
           }
       }
   }

   // check circularity in inheritance graph
   int sectionId = findCycle(graph);
   if (sectionId != -1)
       throw cRuntimeError("Cycle detected in section fallback chain at [%s]", getSectionName(sectionId));

   // check for inconsistent hierarchy
   for (int i = 0; i < getNumSections(); i++)
       resolveSectionChain(i);
}

cConfigOption *InifileContents::lookupConfigOption(const char *key)
{
    cConfigOption *e = (cConfigOption *)configOptions.getInstance()->lookup(key);
    if (e)
        return e;  // found it, great

    // Maybe it matches on a cConfigOption which has '*' or '%' in its name,
    // such as "seed-1-mt" matches on the "seed-%-mt" cConfigOption.
    // We have to iterate over all cConfigOptions to verify this.
    // "%" means "any number" in config keys.
    int n = configOptions.getInstance()->size();
    for (int i = 0; i < n; i++) {
        cConfigOption *e = (cConfigOption *)configOptions.getInstance()->get(i);
        if (PatternMatcher::containsWildcards(e->getName()) || strchr(e->getName(), '%') != nullptr) {
            std::string pattern = opp_replacesubstring(e->getName(), "%", "{..}", true);
            if (PatternMatcher(pattern.c_str(), false, true, true).matches(key))
                return e;
        }
    }
    return nullptr;
}

bool InifileContents::isIgnorableConfigKey(const char *ignoredKeyPatterns, const char *key)
{
    // see if any element in ignoredKeyPatterns matches it
    StringTokenizer tokenizer(ignoredKeyPatterns ? ignoredKeyPatterns : "");
    while (tokenizer.hasMoreTokens())
        if (PatternMatcher(tokenizer.nextToken(), true, true, true).matches(key))
            return true;

    return false;
}

void InifileContents::dump(bool printBaseDir) const
{
    for (int i = 0; i < getNumSections(); i++) {
        std::cout << "\n[" << getSectionName(i) << "]\n";
        for (int j = 0; j < getNumEntries(i); j++) {
            const Entry& e = getEntry(i, j);
            std::cout << e.getKey() << " = " << e.getValue();
            if (printBaseDir)
                std::cout << "  dir=" << e.getBaseDirectory();
            std::cout << "\n";
        }
    }
}

}  // namespace envir
}  // namespace omnetpp

