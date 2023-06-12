//----------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//----------------------------------------------------------------------------------
// ***************  Documentation  *******************
// This plugin implements the possibility to configure the caching decision.
// It can be used with a regex, or a fixed filelist, or both combined.
// Note: The FileList decision outweighs the regex decision!

// - Regex:
//      The default mode for regex matching is to cache everything that matches the regex, e.g. ".*.root".
//      To invert the decision, we can use "~", e.g. "~.*.root" does not cache if the regex matches.
//      Multiple regexes can be combined with a ",": ".*.root, .*AOD.*, abc123".
//      It is also possible, to combine black- and whitelisting by regex matching, e.g. "~.*AOD*, .*NANO.*".
//      This example reads: do not cache a file that contains "AOD", unless it contains "NANO" (and all others containing "NANO"!).
//      Note: The regex matching is implemented to prefer a positive caching decision over a negative one!
//      This means, "~.*AOD*, .*AOD.*" would lead to a positive caching decision.
//      When no regex is configured, the regex matching is skipped.
//      Note: if the same regex is configured multiple times, the last is used!
//
// - FileList:
//      Additionally, the decision can be made by a fixed filelist containing the full paths, e.g.
//          /store/user/my_awesome.file
//      There are two modes, "blacklist" and "whitelist". Only one can be chosen. In the config, "filepath" (path to
//      filelist) and "filelist_mode" (blacklist/whitelist) both must be set.
//      This can be combined with the regex e.g.:
//          blacklist all files containing "AOD" with ~.*AOD.* except /store/user/my_awesome_AOD_file by whitelisting
//
// *********************  Usage  *********************
// The decision lib to be enabled in the caching server config, e.g.:
//      pfc.decisionlib libXrdCachingDecision.so /path-to-config/cachingdecision.cfg
// Verbose output (printing used regex and filelist) can be enabled with:
//      xrd.trace debug
// It enables:
//      - parsing info
//      - content of regex map and mode
//      - content of filelist and mode
//      - cache decision map
//      - printing of the tested regex per file
//
// *********************  Config  ********************
// The configuration files follow this rules:
// - The config can currently contain the following sections and keys/values:
//     sections:
//       [RegEx]
//       [FileList]
//     keys,values:
//       regex=.*example_regex.*
//       filepath=/path/to/filelist.txt
//       filelist_mode=blacklist or whitelist
// - Unknown sections, keys, and values lead to config errors
// - Multiple definitions in the config file lead to errors, e.g. two times the same key/section
// - Empty keys are ignored, e.g. "regex= "
// - Empty sections are ignored
// - "#" means a comment and the line is ignored
// - " ", "\t", etc are ignored
// - The regex itself must not contain a comma (as it is the separator)
// - Configure regex as blacklisting is done by adding a "~"
// - Regexes can be combined with a comma, e.g. "~.*root, .*NANOAOD.*, <...>"
//
// Other remarks:
// - The sections are actually irrelevant, as they are filled automatically when a certain key is added (e.g. "regex").
// - if something fails, the default is to cache everything
// - if the same regex is configured multiple times (e.g. as positve AND negative), the LAST one is used, e.g.:
//      .*parquet, ~.*parquet -> blacklisting .*parquet
//
// ********************  Examples  *******************
// - cache everything with regex:
//      regex=.*
// - blacklisting AOD but not MiniAOD:
//      regex=~.*AOD.*,.*MINIAOD.*
//
//----------------------------------------------------------------------------------

#include "XrdPfcDecision.hh"
#include "XrdSys/XrdSysError.hh"
#include "Xrd/XrdTrace.hh"

//#include <iostream>  // TODO: necessary?
#include <fstream>
#include <sstream>

#include <regex>
#include <vector>
#include <set>

#include <experimental/filesystem>  // c++14
//#include <filesystem>  //c++17

class CachingDecision : public XrdPfc::Decision
{
//----------------------------------------------------------------------------
//! A decision library that allows advanced caching decisions
//----------------------------------------------------------------------------

public:

virtual bool Decide(const std::string & lfn, XrdOss &) const {
    /*
     * **************************************************************************************
     *  Function for applying the configured decisions.                                     *
     *  First, regex is tested. Then, the filelist is tested. If something is wrong with    *
     *  config, everything is tested.                                                       *
     *  Note: filelist decision always > regex decision !                                   *
     * **************************************************************************************
     */
    TRACE(DEBUG, "Decide: " << "Cache decision map:");
    for (const auto& iter: m_cacheDecision_map) {
        TRACE(DEBUG, "key, value: " << iter.first.c_str() << " " << iter.second.c_str());
    }

    if ( ! m_configSuccess ) {
        m_log.Emsg("Decide", "Configuration failed. Cache EVERYTHING.");
        return true;
    }
    m_log.Emsg("Decide", "Deciding whether to cache file: >>", lfn.c_str(), "<<");
    bool decision = false;

    // 1) check if RegEx caching is configured first, since filelist can override it
    // Note: if the same regex is configured multiple times, the LAST one is used!
    // And a positive decision always > negative decision, as it directly breaks the loop
    if ( ( ! m_regExpList_map.empty() ) && ( m_cacheDecision_map.at("RegEx") == "enabled" ) ) {
        for (const auto &rx: m_regExpList_map) {
            if ( std::regex_match(lfn, std::regex(rx.first)) ) {
                TRACE(DEBUG, "Testing: " << rx.first.c_str() << " " << rx.second.c_str());
                if ( rx.second == "whitelist" ) {
                    m_log.Emsg("Decide", "RegEx whitelist match for: >>", lfn.c_str(), "<<");
                    decision = true;
                    break;
                }
                else if ( rx.second == "blacklist" ) {
                    m_log.Emsg("Decide", "RegEx blacklist match for: >>", lfn.c_str(), "<<");
                    //break;  // regex could be overridden -> do not break / comment in if this behaviour is not desired
                }
                else {  // should not happen
                    m_log.Emsg("Decide", "+++++ RegEx error +++++ Unknown mode for: >>",
                               rx.first.c_str(), "<<. Ignored.");
                }
            }
        }
        if ( ! decision ) {
            m_log.Emsg("Decide", "RegEx decision: do not cache >>", lfn.c_str(), "<<");  // can be overridden!
        }
    }
    // 2) check if filelist caching is configured
    // Note: No "else if", since the filelist decision can override the regex decision!
    if ( ( ! m_filelist.empty() ) && ( m_cacheDecision_map.at("FileList") == "enabled" ) ) {
        if ( m_filelist.find(lfn) != m_filelist.end() ) {
            if ( ( m_cacheDecision_map.at("filelist_mode") == "whitelist" ) ) {
                m_log.Emsg("Decide", "FileList whitelist match: >>", lfn.c_str(), "<<");
                decision = true;
            } else if ( ( m_cacheDecision_map.at("filelist_mode") == "blacklist" ) ) {
                m_log.Emsg("Decide", "FileList blacklist match: >>", lfn.c_str(), "<<");
                decision = false;
            } else {
                m_log.Emsg("Decide", "+++ Error +++ Unknown mode: >>",
                           m_cacheDecision_map.at("filelist_mode").c_str(), "<< Cache EVERYTHING");
                decision = true;
            }
        }
        else {
            m_log.Emsg("Decide", "No FileList match for: >>", lfn.c_str(), "<<");
        }
    }
    // empty config / something went wrong?
    if ( m_filelist.empty() && m_regExpList_map.empty() ) {
        m_log.Emsg("Decide", "Decision list empty. Please review your config. This could be an ERROR... Cache EVERYTHING");
        decision = true;
    }

    if ( decision ) {
        m_log.Emsg("Decide", "File >>", lfn.c_str(), "<< will be cached.");
    }
    else {
        m_log.Emsg("Decide", "File >>", lfn.c_str(), "<< not cached.");
    }

    return decision;
}

explicit CachingDecision(XrdSysError &log)
        : m_log(log)
{}

virtual bool ConfigDecision(const char * params) {
    /*
     * **************************************************************************************
     *  Function to configure caching decisions from file.                                  *
     *  Note: if no config file is given (-> params is empty), this function is not called! *
     * **************************************************************************************
     */
    m_log.Emsg("ConfigDecision", "+++++ Init decision library +++++");
    m_params_str = std::string(params);

    // check if configfile is given as params
    const std::experimental::filesystem::path path(params);
    std::error_code ec;
    if ( std::experimental::filesystem::is_regular_file(params, ec) ) {
        // if config file is provided, parse file
        m_log.Emsg("ConfigDecision", "Using config file: >>", params, "<<");

        bool parserSuccess = parseConfig(m_params_str, m_cacheDecision_map);
        if (parserSuccess) {  // filling of cache decision data
            // returning false -> m_configSuccess = false; -> cache everything!

            // if regex is configured in file:
            auto it = m_cacheDecision_map.find("regex");
            if (it != m_cacheDecision_map.end()) {
                bool regexSuccess = fillRegExList(it->second);
                if ( ! regexSuccess ) {
                    m_log.Emsg("ConfigDecision", "Regex decision configuration failed!");
                    return false;
                }
                m_log.Emsg("ConfigDecision", "RegEx decision configured.");
                m_cacheDecision_map["RegEx"] = "enabled";  // for checking if configured
            } else {
                m_log.Emsg("ConfigDecision", "No RegEx decision configured.");
                m_cacheDecision_map["RegEx"] = "disabled";
            }

            // if filelist is configured in file and filelist_mode is selected:
            it = m_cacheDecision_map.find("filepath");
            if ((it != m_cacheDecision_map.end()) && (m_cacheDecision_map.count("filelist_mode") == 1)) {
                bool filelistSuccess = fillFileList(it->second);
                TRACE(DEBUG, "FileList mode: " << m_cacheDecision_map["filelist_mode"])
                if ( ! filelistSuccess ) {
                    m_log.Emsg("ConfigDecision", "FileList decision configuration failed!");
                    return false;
                }
                m_log.Emsg("ConfigDecision", "FileList decision configured.");
                m_cacheDecision_map["FileList"] = "enabled";  // for checking if configured
            }
            else {
                m_log.Emsg("ConfigDecision", "No FileList decision configured / filepath or mode missing/unknown.");
                m_cacheDecision_map["FileList"] = "disabled";
            }
        }
        else {  // parser error -> m_configSuccess = false -> cache everything
            return false;
        }
    }
    else {
        m_log.Emsg("ConfigDecision", "Configuration failed.");
    }
    if ( ec ) {  // error reporting for first "if" case
        m_log.Emsg("ConfigDecision", "Config file read error: ", ec.message().c_str());
        return false;
    }

    m_log.Emsg("ConfigDecision", "+++++ Init finished. +++++");
    m_configSuccess = true;

    return true;
}

bool parseConfig(const std::string& configFile, std::map<std::string, std::string>& cacheDecisionData) {
    /*
     * **************************************************************
     * Helper function to parse a toml-like config into a std::map. *
     * All sections and keys must be configured only once!          *
     * **************************************************************
     */
    std::ifstream inputFile(configFile);
    if ( ! inputFile.is_open() ) {
        m_log.Emsg("parseConfig", "Failed to open config file: >>", configFile.c_str(), "<<");
        return false;
    }

    TRACE(DEBUG, "parseConfig: " << "The following sections are read in:");
    std::string line;
    std::string section;
    while (std::getline(inputFile, line)) {
        if ( line.empty() || line[0] == '#' ) {  // skip empty lines and comments
            continue;
        }

        if ( line[0] == '[' && line[line.size() - 1] == ']' ) {  // section header
            section = line.substr(1, line.size() - 2);
            TRACE(DEBUG, ">>" << section << "<<");

            // read in section
            if ( ( section == "RegEx" ) || ( section == "FileList" ) ) {
                if ( cacheDecisionData.find(section) == cacheDecisionData.end() ) {
                    cacheDecisionData[section] = "";  // just to check if already read in
                }
                else {
                    m_log.Emsg("parseConfig", "+++Parsing error+++ The section >>", section.c_str(),
                               "<< already exists.");
                    return false;
                }
            }
            else {  // Unknown sections
                m_log.Emsg("parseConfig", "+++Parsing error+++ Unknown section >>", section.c_str(),
                           "<< Please review the config.");
                return false;
            }
        }
        else {  // key-value pair
            size_t equalPos = line.find('=');
            if ( equalPos == std::string::npos ) {
                m_log.Emsg("parseConfig", "+++Parsing error+++ Invalid config:", line.c_str());
                return false;
            }

            std::string key = line.substr(0, equalPos);
            std::string value = line.substr(equalPos + 1);

            // Remove leading/trailing whitespaces
            key.erase(0, key.find_first_not_of(" \t\n\r\f\v"));
            key.erase(key.find_last_not_of(" \t\n\r\f\v") + 1);
            value.erase(0, value.find_first_not_of(" \t\n\r\f\v"));
            value.erase(value.find_last_not_of(" \t\n\r\f\v") + 1);

            // read in key-value pairs
            if ( ( key == "regex" ) || ( key == "filelist_mode" ) || ( key == "filepath" ) ) {
                if ( cacheDecisionData.find(key) == cacheDecisionData.end() ) {
                    if ( ! value.empty() ) {  // only fill, if value is given
                        cacheDecisionData[key] = value;
                    }
                    else {
                        m_log.Emsg("parseConfig", "+++ Attention +++ Empty key: >>", key.c_str(),
                                   "<< ignored.");
                        continue;
                    }
                }
                else {
                    m_log.Emsg("parseConfig", "+++Parsing error+++ The key >>", key.c_str(),
                               "<< already exists.");
                    return false;
                }
            }
            else {
                m_log.Emsg("parseConfig", "+++Parsing error+++ Unknown key >>", key.c_str(),
                           "<< ignored.");
                //return false;  // Unknown keys are ignored!
            }
        }
    }
    inputFile.close();

    return true;
}

bool fillFileList(const std::string& filename) {
    /*
     * **********************************************************************************
     * Helper function to read in a filelist configured in the config file as filepath. *
     * **********************************************************************************
     */
    std::ifstream file(filename, std::ios::in);
    if ( ! file ){
        m_log.Emsg("fillFileList", "Error while opening: >>", filename.c_str(), "<<");
        return false;
    }
    m_log.Emsg("fillFileList", "The following FileList will be used: >>", filename.c_str(), "<<");

    // read file buffered
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string fileContent = buffer.str();

    std::regex regColon("\n");
    std::sregex_token_iterator fileContIt(fileContent.begin(), fileContent.end(), regColon, -1);
    const std::sregex_token_iterator fileContEndIt;

    while (fileContIt != fileContEndIt){
        m_filelist.insert(*fileContIt++);
    }
    m_log.Emsg("fillFileList", "FileList successfully filled.");

    // Debug output
    TRACE(DEBUG, "FileList content:")
    for (const auto &it: m_filelist){
        TRACE(DEBUG, "file: " << it)
    }

    return true;
}

bool fillRegExList(const std::string& rx) {
    /*
     * *********************************************************************
     * Helper function to fill a RegEx from config file into m_regExpList. *
     * *********************************************************************
     */
    m_log.Emsg("fillRegExList", "The following RegExes will be used: >>", rx.c_str(),
               "<<  (Note: '~' indicates blacklisting.)");

    std::regex configStringSeparator(",");
    std::vector<std::string> regExpList_vec(std::sregex_token_iterator(rx.begin(), rx.end(), configStringSeparator, -1),
                                            std::sregex_token_iterator());

    for (auto &pattern: regExpList_vec) {  // not const for removing white spaces!
        try {
            pattern.erase(0, pattern.find_first_not_of(" \t\n\r\f\v"));
            if ( ! pattern.empty() ) {
                std::regex testRegex(pattern);  // try if valid regex
                TRACE(DEBUG, "Fill: " << pattern)
                if ( pattern[0] == '~' ) {  // if first char is "~", blacklist match
                    m_regExpList_map[pattern.substr(1, pattern.length() - 1)] = "blacklist";
                } else {  // else whitelist
                    m_regExpList_map[pattern] = "whitelist";
                }
            }
        } catch (const std::regex_error &) {
            m_log.Emsg("fillRegExList", "+++Error+++ Not a valid RegEx: >>", pattern.c_str(), "<<");
            return false;
        }
    }
    if ( m_regExpList_map.empty() ) {
        m_log.Emsg("fillRegExList", "RegEx list empty.");
    }
    else {
        m_log.Emsg("fillRegExList", "RegEx list successfully filled.");
    }

    // Debug output
    TRACE(DEBUG, "Used Regexes:")
    for (const auto &it: m_regExpList_map){
        TRACE(DEBUG, "RegEx: " << it.first << " mode: " << it.second)
    }

    return true;
}

private:
    std::string m_params_str;
    std::set<std::string> m_filelist;
    XrdSysError &m_log;
    std::map<std::string, std::string> m_regExpList_map;  // pattern:mode
    std::map<std::string, std::string> m_cacheDecision_map;
    bool m_configSuccess = false;

    static const char *TraceID;
};

// Globals: necessary for tracing (verbose debug output of filelist/ regexes)
const char *CachingDecision::TraceID= "CachingDecision_DEBUG";

/******************************************************************************/
/*                          XrdPfcGetDecision                                 */
/******************************************************************************/

// Return a decision object to use.
extern "C"
{
XrdPfc::Decision *XrdPfcGetDecision(XrdSysError &err)
{
   return new CachingDecision(err);
}

}