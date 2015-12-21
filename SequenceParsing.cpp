/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2015 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#include "SequenceParsing.h"

#include <cassert>
#include <cmath>
#include <climits>
#include <cctype> // isdigit(c)
#include <cstddef>
#ifdef DEBUG
#include <iostream>
#endif
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <locale>
#include <istream>
#include <algorithm>
#include <memory>

#include "tinydir/tinydir.h"


// Use: #pragma message WARN("My message")
#if _MSC_VER
#   define FILE_LINE_LINK __FILE__ "(" STRINGISE(__LINE__) ") : "
#   define WARN(exp) (FILE_LINE_LINK "WARNING: " exp)
#else//__GNUC__ - may need other defines for different compilers
#   define WARN(exp) ("WARNING: " exp)
#endif

///the maximum number of non existing frame before Natron gives up trying to figure out a sequence layout.
#define NATRON_DIALOG_MAX_SEQUENCES_HOLE 1000

using std::size_t;

namespace  {
    
#if 0
    // case-insensitive char_traits
    // see http://www.gotw.ca/gotw/029.htm
    // in order to use case insensitive search and compare, all functions should
    // be templated and use basic_string<>:
    // template<class _CharT, class _Traits, class _Allocator>
    // void function(basic_string<_CharT, _Traits, _Allocator>& s, ...)
    //
    struct ci_char_traits : public std::char_traits<char> {
        static bool eq(char c1, char c2) { return toupper(c1) == toupper(c2); }
        static bool ne(char c1, char c2) { return toupper(c1) != toupper(c2); }
        static bool lt(char c1, char c2) { return toupper(c1) <  toupper(c2); }
        static int compare(const char* s1, const char* s2, size_t n) {
            while( n-- != 0 ) {
                if( toupper(*s1) < toupper(*s2) ) return -1;
                if( toupper(*s1) > toupper(*s2) ) return 1;
                ++s1; ++s2;
            }
            return 0;
        }
        static const char* find(const char* s, int n, char a) {
            while( n-- > 0 && toupper(*s) != toupper(a) ) {
                ++s;
            }
            return s;
        }
    };
    
    typedef std::basic_string<char, ci_char_traits> ci_string;
#endif
    
    /**
     * @brief Given the pattern unpathed without extension (e.g: "filename###") and the file extension (e.g "jpg") ; this
     * functions extracts the common parts and the variables of the pattern ordered from left to right .
     * For example: file%04dname### and the jpg extension would return:
     * 3 common parts: "file","name",".jpg"
     * 2 variables: "%04d", "###"
     * The variables by order vector's second member is an int indicating how many non-variable (chars belonging to common parts) characters
     * were found before this variable.
     **/
    static bool extractCommonPartsAndVariablesFromPattern(const std::string& patternUnPathedWithoutExt,
                                                          const std::string& patternExtension,
                                                          StringList* commonParts,
                                                          std::vector<std::pair<std::string,int> >* variablesByOrder)
    {
        bool inPrintfLikeArg = false;
        int printfLikeArgIndex = 0;
        std::string commonPart;
        std::string variable;
        int commonCharactersFound = 0;
        bool previousCharIsSharp = false;
        for (int i = 0; i < (int)patternUnPathedWithoutExt.size(); ++i) {
            const char& c = patternUnPathedWithoutExt[i];
            if (c == '#') {
                if (!commonPart.empty()) {
                    commonParts->push_back(commonPart);
                    commonCharactersFound += commonPart.size();
                    commonPart.clear();
                }
                if (!previousCharIsSharp && !variable.empty()) {
                    variablesByOrder->push_back(std::make_pair(variable,commonCharactersFound));
                    variable.clear();
                }
                variable.push_back(c);
                previousCharIsSharp = true;
            } else if (c == '%') {
                
                char next = '\0';
                if (i < (int)patternUnPathedWithoutExt.size() - 1) {
                    next = patternUnPathedWithoutExt[i + 1];
                }
                char prev = '\0';
                if (i > 0) {
                    prev = patternUnPathedWithoutExt[i - 1];
                }
                
                if (next == '\0') {
                    ///if we're at end, just consider the % character as any other
                    commonPart.push_back(c);
                } else if (prev == '%') {
                    ///we escaped the previous %, append this one to the text
                    commonPart.push_back(c);
                } else if (next != '%') {
                    ///if next == % then we have escaped the character
                    ///we don't support nested  variables
                    if (inPrintfLikeArg) {
                        return false;
                    }
                    printfLikeArgIndex = 0;
                    inPrintfLikeArg = true;
                    if (!commonPart.empty()) {
                        commonParts->push_back(commonPart);
                        commonCharactersFound += commonPart.size();
                        commonPart.clear();
                    }
                    if (!variable.empty()) {
                        variablesByOrder->push_back(std::make_pair(variable,commonCharactersFound));
                        variable.clear();
                    }
                    variable.push_back(c);
                }
            } else if ((c == 'd' || c == 'v' || c == 'V')  && inPrintfLikeArg) {
                inPrintfLikeArg = false;
                assert(!variable.empty());
                variable.push_back(c);
                variablesByOrder->push_back(std::make_pair(variable,commonCharactersFound));
                variable.clear();
            } else if (inPrintfLikeArg) {
                ++printfLikeArgIndex;
                assert(!variable.empty());
                variable.push_back(c);
                ///if we're after a % character, and c is a letter different than d or v or V
                ///or c is digit different than 0, then we don't support this printf like style.
                if (std::isalpha(c,std::locale()) ||
                    (printfLikeArgIndex == 1 && c != '0')) {
                    commonParts->push_back(variable);
                    commonCharactersFound += variable.size();
                    variable.clear();
                    inPrintfLikeArg = false;
                }
                
            } else {
                commonPart.push_back(c);
                if (!variable.empty()) {
                    variablesByOrder->push_back(std::make_pair(variable,commonCharactersFound));
                    variable.clear();
                }
            }
        }
        
        if (!commonPart.empty()) {
            commonParts->push_back(commonPart);
            commonCharactersFound += commonPart.size();
        }
        if (!variable.empty()) {
            variablesByOrder->push_back(std::make_pair(variable,commonCharactersFound));
        }
        
        if (!patternExtension.empty()) {
            commonParts->push_back(std::string('.' + patternExtension));
        }
        return true;
    }
    
    
    static size_t findStr(const std::string& from,
                          const std::string& toSearch,
                          int pos)
    {
        return from.find(toSearch, pos);
        // case insensitive version:
        //return ci_string(from.c_str()).find(toSearch.c_str(), pos);
    }
    
    
    static bool startsWith(const std::string& str,
                           const std::string& prefix)
    {
        return str.substr(0,prefix.size()) == prefix;
        // case insensitive version:
        //return ci_string(str.substr(0,prefix.size()).c_str()) == prefix.c_str();
    }
    
    static bool endsWith(const std::string &str, const std::string &suffix)
    {
        return ((str.size() >= suffix.size()) &&
                (str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0));
    }
    
    static void removeAllOccurences(std::string& str,
                                    const std::string& toRemove)
    {
        if (str.size()) {
            for (size_t i = findStr(str, toRemove, 0);
                 i != std::string::npos;
                 i = findStr(str, toRemove, i)) {
                str.erase(i,toRemove.size());
            }
        }
    }
    
    static int stringToInt(const std::string& str)
    {
        //    std::stringstream ss(str);
        //    int ret = 0;
        //    try {
        //        ss >> ret;
        //    } catch (const std::ios_base::failure& e) {
        //        return 0;
        //    }
        //    return ret;
        return std::atoi(str.c_str());
    }
    
    static std::string stringFromInt(int nb)
    {
        std::stringstream ss;
        ss << nb;
        return ss.str();
    }
    
    static std::string removeFileExtension(std::string& filename)
    {
        size_t lastdot = filename.find_last_of(".");
        if (lastdot == std::string::npos) {
            return "";
        }
        std::string extension = filename.substr(lastdot + 1);
        filename = filename.substr(0, lastdot);
        return extension;
    }
    
    static void getFilesFromDir(tinydir_dir& dir,StringList* ret)
    {
        ///iterate through all the files in the directory
        while (dir.has_next) {
            tinydir_file file;
            tinydir_readfile(&dir, &file);
            
            if (file.is_dir) {
                tinydir_next(&dir);
                continue;
            }
            
            std::string filename(file.name);
            if (filename == "." || filename == "..") {
                tinydir_next(&dir);
                continue;
            } else {
                ret->push_back(filename);
            }
            
            tinydir_next(&dir);
            
        }
    }
    
    /*
     The following rules applying for matching frame numbers:
     - If the number has at least as many digits as the digitsCount then it is OK
     - If the number has more digits than the digitsCOunt, it is only OK if it has 0 prepending zeroes
     */
    static bool numberMatchDigits(int digitsCount,const std::string number,int *frameNumber)
    {
        
        *frameNumber = stringToInt(number);
        
        if ((int)number.size() == digitsCount) {
            return true;
        }
        
        if ((int)number.size() < digitsCount) {
            return false;
        }
        
        assert((int)number.size() > digitsCount);
        
        int nbPrependingZeroes = 0;
        
        std::string noStrWithoutZeroes;
        for (std::size_t i = 0; i < number.size(); ++i) {
            if (number[i] != '0') {
                break;
            }
            ++nbPrependingZeroes;
        }
        if (nbPrependingZeroes == (int)number.size()) {
            --nbPrependingZeroes;
        }
        
        assert(nbPrependingZeroes >= 0);
        
        
        if (nbPrependingZeroes > 0) {
            return false;
        }
        
        return true;
    }
    
    static bool matchesHashTag(int sharpCount,
                               const std::string& filename,
                               size_t startingPos,
                               size_t *endPos,
                               int* frameNumber)
    {
        std::string variable;
        size_t variableIt = startingPos;
        for (variableIt = startingPos;
             variableIt < filename.size() && std::isdigit(filename[variableIt]);
             ++variableIt) {
            variable.push_back(filename[variableIt]);
        }
        *endPos = variableIt;
        
        return numberMatchDigits(sharpCount, variable, frameNumber);
        
        
    }
    
    static bool matchesPrintfLikeSyntax(int digitsCount,
                                        const std::string& filename,
                                        size_t startingPos,
                                        size_t *endPos,
                                        int* frameNumber)
    {
        std::string variable;
        size_t variableIt;
        for (variableIt = startingPos;
             variableIt < filename.size() && std::isdigit(filename[variableIt]);
             ++variableIt) {
            variable.push_back(filename[variableIt]);
        }
        *endPos = variableIt;
        
        return numberMatchDigits(digitsCount, variable, frameNumber);
        
    }
    
    static bool matchesView(bool longView,
                            const std::string& filename,
                            size_t startingPos,
                            size_t *endPos,
                            int* viewNumber)
    {
        std::string mid = filename.substr(startingPos);
        if (!longView) {
            
            if (startsWith(mid,"r")) {
                *viewNumber = 1;
                *endPos = startingPos + 1;
                return true;
            } else if (startsWith(mid,"l")) {
                *viewNumber = 0;
                *endPos = startingPos + 1;
                return true;
            } else if (startsWith(mid,"view")) {
                std::string viewNoStr;
                for (size_t it = 4; it < mid.size() && std::isdigit(mid[it]); ++it) {
                    viewNoStr.push_back(mid[it]);
                }
                if (!viewNoStr.empty()) {
                    *viewNumber = stringToInt(viewNoStr);
                    *endPos = startingPos + 4 + viewNoStr.size();
                } else {
                    return false;
                }
                return true;
            }
            return false;
        } else {
            if (startsWith(mid,"right")) {
                *viewNumber = 1;
                *endPos = startingPos + 5;
                return true;
            } else if (startsWith(mid,"left")) {
                *viewNumber = 0;
                *endPos = startingPos + 4;
                return true;
            } else if (startsWith(mid,"view")) {
                std::string viewNoStr;
                for (size_t it = 4; it < mid.size() && std::isdigit(mid[it]); ++it) {
                    viewNoStr.push_back(mid[it]);
                }
                
                if (!viewNoStr.empty()) {
                    *viewNumber = stringToInt(viewNoStr);
                    *endPos = startingPos + 4 + viewNoStr.size();
                } else {
                    return false;
                }
                return true;
            }
            return false;
        }
    }
    
    static bool matchesPattern_v2(const std::string& filename,
                                  const std::string& pattern,
                                  const std::string& patternExtension,
                                  int* frameNumber,
                                  int* viewNumber)
    {
        
        ///If the frame number is found twice or more, this is to verify if they are identical
        bool wasFrameNumberSet = false;
        
        ///If the view number is found twice or more, this is to verify if they are identical
        bool wasViewNumberSet = false;
        
        ///Default view number and frame number
        assert(viewNumber && frameNumber);
        *viewNumber = 0;
        *frameNumber = -1;
        
        ///Iterators on the pattern and the filename
        size_t filenameIt = 0;
        size_t patternIt = 0;
        
        ///make a copy of the filename from which we remove the file extension
        std::string filenameCpy = filename;
        std::string fileExt = removeFileExtension(filenameCpy);
        
        ///Extensions not matching, exit.
        if (fileExt != patternExtension) { //
            return false;
        }
        
        ///Iterating while not at end of either the pattern or the filename
        while (filenameIt < filenameCpy.size() && patternIt < pattern.size()) {
            ///the count of '#' characters found
            int sharpCount = 0;
            
            ///Actually start counting the #
            std::string variable;
            for (size_t sharpIt = patternIt;
                 sharpIt < pattern.size() && pattern[sharpIt] == '#';
                 ++sharpIt) {
                ++sharpCount;
                variable.push_back('#');
            }
            
            ///Did we found a %d style syntax ?
            bool foundPrintFLikeSyntax = false;
            
            ///Did we found a %v style syntax ?
            bool foundShortView = false;
            
            ///Did we found a %V style syntax ?
            bool foundLongView = false;
            
            ///How many digits the printf style %d syntax are desired, e.g %04d is 4
            int printfDigitCount = 0;
            
            ///The number of characters that compose the %04d style variable, this is at least 2 (%d)
            int printfLikeVariableSize = 2;
            if (pattern[patternIt] == '%') {
                ///We found the '%' digit, start at the character right after to
                ///find digits
                size_t printfIt;
                std::string digitStr;
                for (printfIt = patternIt + 1;
                     printfIt < pattern.size() && std::isdigit(pattern[printfIt]);
                     ++printfIt) {
                    digitStr.push_back(pattern[printfIt]);
                    ++printfLikeVariableSize;
                }
                
                ///they are no more digit after the '%', check if this is correctly terminating by a 'd' character.
                /// We also treat the view %v and %V cases here
                if (printfIt < pattern.size() && std::tolower(pattern[printfIt]) == 'd') {
                    foundPrintFLikeSyntax = true;
                    printfDigitCount = stringToInt(digitStr);
                } else if (printfIt < pattern.size() && pattern[printfIt] == 'V') {
                    foundLongView = true;
                } else if (printfIt < pattern.size() && pattern[printfIt] == 'v') {
                    foundShortView = true;
                }
                
            }
            
            
            if (sharpCount > 0) { ///If we found #
                
                ///There cannot be another variable!
                assert(!foundPrintFLikeSyntax && !foundLongView && !foundShortView);
                size_t endHashTag = 0;
                int fNumber = -1;
                
                ///check if the filename matches the number of hashes
                if (!matchesHashTag(sharpCount, filenameCpy, filenameIt, &endHashTag, &fNumber)) {
                    return false;
                }
                
                ///If the frame number had already been set and it was different, this filename doesn't match
                ///the pattern.
                if (wasFrameNumberSet && fNumber != *frameNumber) {
                    return false;
                }
                
                wasFrameNumberSet = true;
                *frameNumber = fNumber;
                
                ///increment iterators to after the variable
                filenameIt = endHashTag;
                patternIt += sharpCount;
                
            } else if (foundPrintFLikeSyntax) { ///If we found a %d style syntax
                
                ///There cannot be another variable!
                assert(sharpCount == 0 && !foundLongView && !foundShortView);
                
                size_t endPrintfLike = 0;
                int fNumber = -1;
                ///check if the filename matches the %d syntax
                if (!matchesPrintfLikeSyntax(printfDigitCount, filenameCpy, filenameIt, &endPrintfLike, &fNumber)) {
                    return false;
                }
                
                ///If the frame number had already been set and it was different, this filename doesn't match
                ///the pattern.
                if (wasFrameNumberSet && fNumber != *frameNumber) {
                    return false;
                }
                
                wasFrameNumberSet = true;
                *frameNumber = fNumber;
                
                ///increment iterators to after the variable
                filenameIt = endPrintfLike;
                patternIt += printfLikeVariableSize;
                
                
            } else if (foundLongView) {
                
                ///There cannot be another variable!
                assert(sharpCount == 0 && !foundPrintFLikeSyntax && !foundShortView);
                
                size_t endVar;
                int vNumber;
                ///check if the filename matches the %V syntax
                if (!matchesView(true, filenameCpy, filenameIt, &endVar, &vNumber)) {
                    return false;
                }
                
                ///If the view number had already been set and it was different, this filename doesn't match
                ///the pattern.
                if (wasViewNumberSet && vNumber != *viewNumber) {
                    return false;
                }
                
                wasViewNumberSet = true;
                *viewNumber = vNumber;
                
                ///increment iterators to after the variable
                filenameIt = endVar;
                patternIt += 2;
                
                
            } else if (foundShortView) {
                
                ///There cannot be another variable!
                assert(sharpCount == 0 && !foundPrintFLikeSyntax && !foundLongView);
                
                size_t endVar;
                int vNumber;
                ///check if the filename matches the %v syntax
                if (!matchesView(false, filenameCpy, filenameIt, &endVar, &vNumber)) {
                    return false;
                }
                
                ///If the view number had already been set and it was different, this filename doesn't match
                ///the pattern.
                if (wasViewNumberSet && vNumber != *viewNumber) {
                    return false;
                }
                
                wasViewNumberSet = true;
                *viewNumber = vNumber;
                
                ///increment iterators to after the variable
                filenameIt = endVar;
                patternIt += 2;
            } else {
                
                ///we found nothing, just compare the characters
                if (pattern[patternIt] != filenameCpy[filenameIt]) {
                    return false;
                }
                ++patternIt;
                ++filenameIt;
            }
        }
        
        bool fileNameAtEnd =  filenameIt >= filenameCpy.size();
        bool patternAtEnd =  patternIt >= pattern.size();
        if (!fileNameAtEnd || !patternAtEnd) {
            return false;
        }
        
        return true ;
    }
    
    
}


namespace SequenceParsing {
    /**
     * @brief A small structure representing an element of a file name.
     * It can be either a text part, or a view part or a frame number part.
     **/
    struct FileNameElement {
        
        enum Type { TEXT = 0  , FRAME_NUMBER };
        
        FileNameElement(const std::string& data,FileNameElement::Type type)
        : data(data)
        , type(type)
        {}
        
        std::string data;
        Type type;
    };
    
    
    ////////////////////FileNameContent//////////////////////////
    
    struct FileNameContentPrivate {
        ///Ordered from left to right, these are the elements composing the filename without its path
        std::vector<FileNameElement> orderedElements;
        std::string absoluteFileName;
        std::string filePath; //< the filepath
        std::string filename; //< the filename without path
        std::string extension; //< the file extension
        std::string generatedPattern;
        int prependingZeroes;
        
        FileNameContentPrivate()
        : orderedElements()
        , absoluteFileName()
        , filePath()
        , filename()
        , extension()
        , generatedPattern()
        , prependingZeroes(0)
        {
        }
        
        void parse(const std::string& absoluteFileName);
        
        static int getPrependingZeroes(const std::string& str);
    };
    
    
    FileNameContent::FileNameContent(const std::string& absoluteFilename)
    : _imp(new FileNameContentPrivate())
    
    {
        _imp->parse(absoluteFilename);
    }
    
    FileNameContent::FileNameContent(const FileNameContent& other)
    : _imp(new FileNameContentPrivate())
    {
        *this = other;
    }
    
    FileNameContent::~FileNameContent()
    {
    }
    
    void
    FileNameContent::operator=(const FileNameContent& other)
    {
        _imp->orderedElements = other._imp->orderedElements;
        _imp->absoluteFileName = other._imp->absoluteFileName;
        _imp->filename = other._imp->filename;
        _imp->filePath = other._imp->filePath;
        _imp->extension = other._imp->extension;
        _imp->generatedPattern = other._imp->generatedPattern;
    }
    
    int
    FileNameContentPrivate::getPrependingZeroes(const std::string& str)
    {
        int ret = 0;
        std::size_t i = 0;
        while (i < str.size()) {
            if (str[i] == '0') {
                ++ret;
            } else {
                break;
                
            }
            ++i;
        }
        return ret;
    }
    
    void
    FileNameContentPrivate::parse(const std::string& absoluteFileName)
    {
        this->absoluteFileName = absoluteFileName;
        filename = absoluteFileName;
        filePath = removePath(filename);
        std::locale loc;
        std::string lastNumberStr;
        std::string lastTextPart;
        for (size_t i = 0; i < filename.size(); ++i) {
            const char& c = filename[i];
            if (std::isdigit(c,loc)) {
                lastNumberStr += c;
                if (!lastTextPart.empty()) {
                    orderedElements.push_back(FileNameElement(lastTextPart,FileNameElement::TEXT));
                    lastTextPart.clear();
                }
            } else {
                if (!lastNumberStr.empty()) {
                    orderedElements.push_back(FileNameElement(lastNumberStr,FileNameElement::FRAME_NUMBER));
                    prependingZeroes = getPrependingZeroes(lastNumberStr); //< take into account only the last FRAME_NUMBER
                    lastNumberStr.clear();
                }
                
                lastTextPart.push_back(c);
            }
        }
        
        if (!lastNumberStr.empty()) {
            orderedElements.push_back(FileNameElement(lastNumberStr,FileNameElement::FRAME_NUMBER));
            prependingZeroes = getPrependingZeroes(lastNumberStr); //< take into account only the last FRAME_NUMBER
            lastNumberStr.clear();
        }
        if (!lastTextPart.empty()) {
            orderedElements.push_back(FileNameElement(lastTextPart,FileNameElement::TEXT));
            lastTextPart.clear();
        }
        
        // extension is everything after the last '.'
        size_t lastDotPos = filename.find_last_of('.');
        if (lastDotPos == std::string::npos) {
            extension.clear();
        } else {
            extension = filename.substr(lastDotPos+1);
        }
        
    }
    
    int
    FileNameContent::getNumPrependingZeroes() const
    {
        return _imp->prependingZeroes;
    }
    
    /**
     * @brief Returns the file path, e.g: /Users/Lala/Pictures/ with the trailing separator.
     **/
    const std::string&
    FileNameContent::getPath() const
    {
        return _imp->filePath;
    }
    
    /**
     * @brief Returns the filename without its path.
     **/
    const std::string&
    FileNameContent::fileName() const
    {
        return _imp->filename;
    }
    
    /**
     * @brief Returns the absolute filename as it was given in the constructor arguments.
     **/
    const std::string&
    FileNameContent::absoluteFileName() const
    {
        return _imp->absoluteFileName;
    }
    
    const std::string&
    FileNameContent::getExtension() const
    {
        return _imp->extension;
    }
    
    
    /**
     * @brief Returns the file pattern found in the filename with hash characters style for frame number (i.e: ###)
     **/
    const std::string&
    FileNameContent::getFilePattern(int numHashes) const
    {
        if (_imp->generatedPattern.empty()) {
            ///now build the generated pattern with the ordered elements.
            int numberIndex = 0;
            for (size_t j = 0; j < _imp->orderedElements.size(); ++j) {
                const FileNameElement& e = _imp->orderedElements[j];
                switch (e.type) {
                    case FileNameElement::TEXT:
                        _imp->generatedPattern.append(e.data);
                        break;
                    case FileNameElement::FRAME_NUMBER:
                    {
                        std::string hashStr;
                        for(int c = 0; c < numHashes; ++c) {
                            hashStr.push_back('#');
                        }
                        _imp->generatedPattern.append(hashStr + stringFromInt(numberIndex));
                        ++numberIndex;
                    } break;
                    default:
                        break;
                }
                
            }
        }
        return _imp->generatedPattern;
    }
    
    /**
     * @brief If the filename is composed of several numbers (e.g: file08_001.png),
     * this functions returns the number at index as a string that will be stored in numberString.
     * If Index is greater than the number of numbers in the filename or if this filename doesn't
     * contain any number, this function returns false.
     **/
    bool
    FileNameContent::getNumberByIndex(int index,
                                      std::string* numberString) const
    {
        int numbersElementsIndex = 0;
        for (size_t i = 0; i < _imp->orderedElements.size(); ++i) {
            if (_imp->orderedElements[i].type == FileNameElement::FRAME_NUMBER) {
                if (numbersElementsIndex == index) {
                    *numberString = _imp->orderedElements[i].data;
                    return true;
                }
                ++numbersElementsIndex;
            }
        }
        return false;
    }
    
    int FileNameContent::getPotentialFrameNumbersCount() const
    {
        int count = 0;
        for (size_t i = 0; i < _imp->orderedElements.size(); ++i) {
            if (_imp->orderedElements[i].type == FileNameElement::FRAME_NUMBER) {
                ++count;
            }
        }
        return count;
    }
    
    /**
     * @brief Given the pattern of this file, it tries to match the other file name to this
     * pattern.
     * @param numberIndexToVary [out] In case the pattern contains several numbers (@see getNumberByIndex)
     * this value will be fed the appropriate number index that should be used for frame number.
     * For example, if this filename is myfile001_000.jpg and the other file is myfile001_001.jpg
     * numberIndexToVary would be 1 as the frame number string indentified in that case is the last number.
     * @returns True if it identified 'other' as belonging to the same sequence, false otherwise.
     **/
    bool FileNameContent::matchesPattern(const FileNameContent& other, int* numberIndexToVary) const
    {
        const std::vector<FileNameElement>& otherElements = other._imp->orderedElements;
        if (otherElements.size() != _imp->orderedElements.size()) {
            return false;
        }
        
        ///We only consider the last potential frame number
        ///
        *numberIndexToVary = -1;
        
        int numbersCount = 0;
        for (size_t i = 0; i < _imp->orderedElements.size(); ++i) {
            if (_imp->orderedElements[i].type != otherElements[i].type) {
                return false;
            }
            if (_imp->orderedElements[i].type == FileNameElement::FRAME_NUMBER) {
                
                /*
                 The following rules applying for matching frame numbers:
                 First we generate hashes from this number, then:
                 - If the other number has at least as many digits as the hashes generated then it is OK
                 - If the other number has more digits than the hashes, it is only OK if it has 0 prepening zeroes
                 */
                int hashesCount = (int)_imp->orderedElements[i].data.size();
                int number;
                bool isOK = false;
                if (_imp->orderedElements[i].data.size() > 0 && _imp->orderedElements[i].data[0] != '0' &&
                    otherElements[i].data.size() > 0 && otherElements[i].data[0] != '0') {
                    isOK = true;
                } else {
                    isOK = numberMatchDigits(hashesCount, otherElements[i].data, &number);
                }
                
                
                if (isOK) {
                    *numberIndexToVary = numbersCount;
                }
                
                ++numbersCount;
            } else if (_imp->orderedElements[i].type == FileNameElement::TEXT && _imp->orderedElements[i].data != otherElements[i].data) {
                return false;
            }
        }
        ///strings are identical
        /// we only accept files with 1 varying number
        if (*numberIndexToVary == -1) {
            return false;
        }
        
        /*Code commented-out : In this previous version we would try to find several places in the filename where the
         frame number would vary, such as:
         mySequence###lalala###.jpg  This led to very complicated cases to handle for too much troubles.
         The new version only assumes the frame number index varying is the last number varying.
         */
        
        //    ///find out in the potentialFrameNumbers what is the minimum with pairs and pick it up
        //    /// for example if 1 pair is : < 0001, 802398 > and the other pair is < 01 , 10 > we pick
        //    /// the second one.
        //    std::vector<int> minIndexes;
        //    int minimum = INT_MAX;
        //    for (size_t i = 0; i < potentialFrameNumbers.size(); ++i) {
        //        int thisNumber = stringToInt(potentialFrameNumbers[i].second.first);
        //        int otherNumber = stringToInt(potentialFrameNumbers[i].second.second);
        //        int diff = std::abs(thisNumber - otherNumber);
        //        if (diff < minimum) {
        //            minimum = diff;
        //            minIndexes.clear();
        //            minIndexes.push_back(i);
        //        } else if (diff == minimum) {
        //            minIndexes.push_back(i);
        //        }
        //    }
        //    for (size_t i = 0; i < minIndexes.size(); ++i) {
        //        numberIndexesToVary->push_back(potentialFrameNumbers[minIndexes[i]].first);
        //    }
        return true;
        
    }
    
    void FileNameContent::generatePatternWithFrameNumberAtIndex(int index,int numHashes, std::string* pattern) const
    {
        int numbersCount = 0;
        size_t lastNumberPos = 0;
        std::string indexedPattern = getFilePattern(numHashes);
        for (size_t i = 0; i < _imp->orderedElements.size(); ++i) {
            if (_imp->orderedElements[i].type == FileNameElement::FRAME_NUMBER) {
                lastNumberPos = findStr(indexedPattern, "#",0);
                assert(lastNumberPos != std::string::npos);
                
                size_t endTagPos = lastNumberPos;
                while (endTagPos < indexedPattern.size() && indexedPattern[endTagPos] == '#') {
                    ++endTagPos;
                }
                
                ///assert that the end of the tag is composed of  a digit
                
                if (endTagPos < indexedPattern.size()) {
                    assert(std::isdigit(indexedPattern[endTagPos]));
                }
                
                if (index == numbersCount) {
                    ///remove the index of the tag and keep the tag.
                    if (endTagPos < indexedPattern.size()) {
                        indexedPattern.erase(endTagPos, 1);
                    }
                } else {
                    ///if this is not the number we're interested in to keep the ###, just expand the variable
                    ///replace the whole tag with the original data
                    indexedPattern.replace(lastNumberPos, endTagPos - lastNumberPos + 1, _imp->orderedElements[i].data);
                }
                
                ++numbersCount;
            }
        }
        
        *pattern = getPath() + indexedPattern;
    }
    
    
    std::string removePath(std::string& filename)
    {
        ///find the last separator
        size_t pos = filename.find_last_of('/');
        if (pos == std::string::npos) {
            //try out \\ char
            pos = filename.find_last_of('\\');
        }
        if (pos == std::string::npos) {
            ///couldn't find a path
            return "";
        }
        std::string path = filename.substr(0,pos+1); // + 1 to include the trailing separator
        removeAllOccurences(filename, path);
        return path;
        
    }
    
    
    static bool filesListFromPattern_internal(const std::string& pattern, SequenceParsing::SequenceFromPattern* sequence)
    {
        if (pattern.empty()) {
            return false;
        }
        
        std::string patternUnPathed = pattern;
        std::string patternPath = removePath(patternUnPathed);
        std::string patternExtension = removeFileExtension(patternUnPathed);
        
        //    ///the pattern has no extension, switch the extension and the unpathed part
        //    if (patternUnPathed.empty()) {
        //        patternUnPathed = patternExtension;
        //        patternExtension.clear();
        //    }
        
        tinydir_dir patternDir;
        if (tinydir_open(&patternDir, patternPath.c_str()) == -1) {
            return false;
        }
        
        ///all the interesting files of the pattern directory
        StringList files;
        getFilesFromDir(patternDir, &files);
        tinydir_close(&patternDir);
        
        for (size_t i = 0; i < files.size(); ++i) {
            int frameNumber;
            int viewNumber;
            if (matchesPattern_v2(files[i], patternUnPathed, patternExtension, &frameNumber, &viewNumber)) {
                SequenceFromPattern::iterator it = sequence->find(frameNumber);
                std::string absoluteFileName = patternPath + files[i];
                if (it != sequence->end()) {
                    std::pair<std::map<int,std::string>::iterator,bool> ret =
                    it->second.insert(std::make_pair(viewNumber,absoluteFileName));
                    if (!ret.second) {
#                 ifdef DEBUG
                        std::cerr << "There was an issue populating the file sequence. Several files with the same frame number"
                        " have the same view index." << std::endl;
#                 endif
                    }
                } else {
                    std::map<int, std::string> viewsMap;
                    viewsMap.insert(std::make_pair(viewNumber, absoluteFileName));
                    sequence->insert(std::make_pair(frameNumber, viewsMap));
                }
            }
        }
        return true;
    }
    
    bool filesListFromPattern(const std::string& pattern, SequenceParsing::SequenceFromPattern* sequence)
    {
        return filesListFromPattern_internal(pattern,sequence);
    }
    
    StringList sequenceFromPatternToFilesList(const SequenceParsing::SequenceFromPattern& sequence, int onlyViewIndex)
    {
        StringList ret;
        for (SequenceParsing::SequenceFromPattern::const_iterator it = sequence.begin(); it != sequence.end(); ++it) {
            const std::map<int,std::string>& views = it->second;
            
            for (std::map<int,std::string>::const_iterator it2 = views.begin(); it2!=views.end(); ++it2) {
                if (onlyViewIndex != -1 && it2->first != onlyViewIndex && it2->first != -1) {
                    continue;
                }
                ret.push_back(it2->second);
            }
        }
        return ret;
    }
    
    std::string generateFileNameFromPattern(const std::string& pattern,
                                            int frameNumber,
                                            int viewNumber)
    {
        std::string patternUnPathed = pattern;
        std::string patternPath = removePath(patternUnPathed);
        std::string patternExtension = removeFileExtension(patternUnPathed);
        
        ///the pattern has no extension, switch the extension and the unpathed part
        if (patternUnPathed.empty()) {
            patternUnPathed = patternExtension;
            patternExtension.clear();
        }
        ///this list represents the common parts of the filename to find in a file in order for it to match the pattern.
        StringList commonPartsToFind;
        ///this list represents the variables ( ###  %04d %v etc...) found in the pattern ordered from left to right in the
        ///original string.
        std::vector<std::pair<std::string,int> > variablesByOrder;
        extractCommonPartsAndVariablesFromPattern(patternUnPathed, patternExtension, &commonPartsToFind, &variablesByOrder);
        
        std::string output = pattern;
        size_t lastVariablePos = std::string::npos;
        for (size_t i = 0; i < variablesByOrder.size(); ++i) {
            const std::string& variable = variablesByOrder[i].first;
            lastVariablePos = findStr(output, variable, lastVariablePos != std::string::npos ? lastVariablePos : 0);
            
            ///if we can't find the variable that means extractCommonPartsAndVariablesFromPattern is bugged.
            assert(lastVariablePos != std::string::npos);
            
            if (variable.find_first_of('#') != std::string::npos) {
                std::string frameNoStr = stringFromInt(frameNumber);
                ///prepend with extra 0's
                while (frameNoStr.size() < variable.size()) {
                    frameNoStr.insert(0,1,'0');
                }
                output.replace(lastVariablePos, variable.size(), frameNoStr);
            } else if (variable.find("%v") != std::string::npos) {
                std::string viewNumberStr;
                if (viewNumber == 0) {
                    viewNumberStr = "l";
                } else if (viewNumber == 1) {
                    viewNumberStr = "r";
                } else {
                    viewNumberStr = "view" + stringFromInt(viewNumber);
                }
                
                output.replace(lastVariablePos,variable.size(), viewNumberStr);
            } else if (variable.find("%V") != std::string::npos) {
                std::string viewNumberStr;
                if (viewNumber == 0) {
                    viewNumberStr = "left";
                } else if (viewNumber == 1) {
                    viewNumberStr = "right";
                } else {
                    viewNumberStr = "view" + stringFromInt(viewNumber);
                }
                
                output.replace(lastVariablePos, variable.size(), viewNumberStr);
            } else if(startsWith(variable, "%0") && endsWith(variable,"d")) {
                std::string digitsCountStr = variable;
                removeAllOccurences(digitsCountStr,"%0");
                removeAllOccurences(digitsCountStr,"d");
                int digitsCount = stringToInt(digitsCountStr);
                std::string frameNoStr = stringFromInt(frameNumber);
                //prepend with extra 0's
                while ((int)frameNoStr.size() < digitsCount) {
                    frameNoStr.insert(0,1,'0');
                }
                output.replace(lastVariablePos, variable.size(), frameNoStr);
            } else if (variable == "%d") {
                output.replace(lastVariablePos, variable.size(), stringFromInt(frameNumber));
                
            } else {
                throw std::invalid_argument("Unrecognized pattern: " + pattern);
            }
        }
        return output;
    }
    
    struct SequenceFromFilesPrivate
    {
        
        ///all the files mapped to their index
        std::map<int,FileNameContent> filesMap;
        
        /// The index of the frame number string in case there're several numbers in a filename.
        int frameNumberStringIndex;
        
        unsigned long long totalSize;
        
        bool sizeEstimationEnabled;
        
        int minNumHashes; //< the minimum number of hash tags # for the pattern
        
        SequenceFromFilesPrivate(bool enableSizeEstimation)
        
        : filesMap()
        , frameNumberStringIndex(-1)
        , totalSize(0)
        , sizeEstimationEnabled(enableSizeEstimation)
        , minNumHashes(0)
        {
            
        }
        
        bool isInSequence(int index) const {
            return filesMap.find(index) != filesMap.end();
        }
    };
    
    SequenceFromFiles::SequenceFromFiles(bool enableSizeEstimation)
    : _imp(new SequenceFromFilesPrivate(enableSizeEstimation))
    {
        
    }
    
    SequenceFromFiles::SequenceFromFiles(const FileNameContent& firstFile,  bool enableSizeEstimation)
    : _imp(new SequenceFromFilesPrivate(enableSizeEstimation))
    {
        tryInsertFile(firstFile);
    }
    
    SequenceFromFiles::~SequenceFromFiles()
    {
    }
    
    SequenceFromFiles::SequenceFromFiles(const SequenceFromFiles& other)
    : _imp(new SequenceFromFilesPrivate(false))
    {
        *this = other;
    }
    
    void
    SequenceFromFiles::operator=(const SequenceFromFiles& other) const
    {
        _imp->filesMap = other._imp->filesMap;
        _imp->frameNumberStringIndex = other._imp->frameNumberStringIndex;
        _imp->totalSize = other._imp->totalSize;
        _imp->sizeEstimationEnabled = other._imp->sizeEstimationEnabled;
    }
    
    bool SequenceFromFiles::tryInsertFile(const FileNameContent& file, bool checkPath) {
        
        if (_imp->filesMap.empty()) {
            ///Special case when the sequence is empty, we don't have anything to match against.
            std::string frameNumberStr;
            _imp->frameNumberStringIndex = file.getPotentialFrameNumbersCount() - 1;
            int frameNumber = -1;
            bool ok = file.getNumberByIndex(_imp->frameNumberStringIndex, &frameNumberStr);
            if (ok) {
                frameNumber = stringToInt(frameNumberStr);
            }
            _imp->filesMap.insert(std::make_pair(frameNumber, file));
            
            _imp->minNumHashes = (int)frameNumberStr.size();
            
            if (_imp->sizeEstimationEnabled) {
                std::ifstream f(file.absoluteFileName().c_str(), std::ios::binary | std::ios::ate);
                _imp->totalSize += f.tellg();
            }
            return true;
        }
        
        assert(!_imp->filesMap.empty());
        
        const  FileNameContent& firstFileContent = _imp->filesMap.begin()->second;
        
        
        if (checkPath && file.getPath() != firstFileContent.getPath()) {
            return false;
        }
        
        ///If the filename has several numbers, such as toto11_titi22.jpg
        ///This corresponds to the index of the number in which the frame number is expected to be.
        ///e.g in this example if 22 would be the frame number, frameNumberIndex would be 1.
        int frameNumberIndex;
        
        bool insert = false;
        if (file.matchesPattern(firstFileContent, &frameNumberIndex)) {
            
            if (frameNumberIndex == _imp->frameNumberStringIndex) {
                
                insert = true;
                
                std::string frameNumberStr;
                bool ok = file.getNumberByIndex(_imp->frameNumberStringIndex, &frameNumberStr);
                if (ok) {
                    
                    
                    std::pair<std::map<int,FileNameContent>::iterator,bool> success =
                    _imp->filesMap.insert(std::make_pair(stringToInt(frameNumberStr),file));
                    
                    ///Insert might have failed because we didn't check prior to this whether the file was already
                    ///present or not.
                    if (success.second) {
                        if (_imp->sizeEstimationEnabled) {
                            std::ifstream f(file.absoluteFileName().c_str(), std::ios::binary | std::ios::ate);
                            _imp->totalSize += f.tellg();
                        }
                    } else {
                        return false;
                        
                    }
                    
                } else {
                    return false;
                }
            }
            
        }
        return insert;
    }
    
    bool SequenceFromFiles::contains(const std::string& absoluteFileName) const {
        for (std::map <int, FileNameContent >::const_iterator it = _imp->filesMap.begin(); it != _imp->filesMap.end(); ++it) {
            if (it->second.absoluteFileName() == absoluteFileName) {
                return true;
            }
        }
        return false;
    }
    
    bool SequenceFromFiles::empty() const {
        return _imp->filesMap.empty();
    }
    
    int SequenceFromFiles::count() const {
        return (int)_imp->filesMap.size();
    }
    
    bool
    SequenceFromFiles::isSingleFile() const
    {
        return _imp->filesMap.size() == 1;
    }
    
    int
    SequenceFromFiles::getFirstFrame() const
    {
        if (_imp->filesMap.empty()) {
            return INT_MIN;
        } else {
            return _imp->filesMap.begin()->first;
        }
    }
    
    int
    SequenceFromFiles::getLastFrame() const
    {
        if (_imp->filesMap.empty()) {
            return INT_MAX;
        } else {
            std::map<int,FileNameContent>::const_iterator it = _imp->filesMap.end();
            --it;
            return it->first;
        }
    }
    
    const std::map<int,FileNameContent>&
    SequenceFromFiles::getFrameIndexes() const
    {
        return _imp->filesMap;
    }
    
    unsigned long long SequenceFromFiles::getEstimatedTotalSize() const {
        return _imp->totalSize;
    }
    
    std::string
    SequenceFromFiles::generateValidSequencePattern() const
    {
        if (empty()) {
            return "";
        }
        if (isSingleFile()) {
            return _imp->filesMap.begin()->second.absoluteFileName();
        }
        assert(_imp->filesMap.size() >= 2);
        std::string firstFramePattern ;
        _imp->filesMap.begin()->second.generatePatternWithFrameNumberAtIndex(_imp->frameNumberStringIndex,
                                                                             _imp->minNumHashes,
                                                                             &firstFramePattern);
        return firstFramePattern;
    }
    
    std::string
    SequenceFromFiles::generateUserFriendlySequencePatternFromValidPattern(const std::string& validPattern) const
    {
        assert(!isSingleFile());
        std::string pattern = validPattern;
        std::vector< std::pair<int,int> > chunks;
        //int first = getFirstFrame();
        std::map<int,FileNameContent>::const_iterator first = _imp->filesMap.begin();
        std::map<int,FileNameContent>::const_iterator cur = first;
        std::map<int,FileNameContent>::const_iterator next = first;
        if (next != _imp->filesMap.end()) {
            ++next;
        }
        while (first != _imp->filesMap.end()) {
            int breakCounter = 0;
            while (next != _imp->filesMap.end() && next->first == (cur->first + 1)) {
                if (next != _imp->filesMap.end()) {
                    ++next;
                }
                if (cur != _imp->filesMap.end()) {
                    ++cur;
                }
                ++breakCounter;
            }
            chunks.push_back(std::make_pair(first->first,cur->first));
            first = next;
            if (next != _imp->filesMap.end()) {
                ++next;
            }
            cur = first;
        }
        
        if (chunks.size() == 1) {
            pattern += ' ';
            pattern += stringFromInt(chunks[0].first);
            pattern += '-';
            pattern += stringFromInt(chunks[0].second);
        } else {
            pattern.append(" ( ");
            for (size_t i = 0 ; i < chunks.size() ; ++i) {
                if (chunks[i].first != chunks[i].second) {
                    pattern += ' ';
                    pattern += stringFromInt(chunks[i].first);
                    pattern += '-';
                    pattern += stringFromInt(chunks[i].second);
                } else {
                    pattern += ' ';
                    pattern += stringFromInt(chunks[i].first);
                }
                if (i < chunks.size() -1) pattern.append(" /");
            }
            pattern.append(" ) ");
        }
        return pattern;

    }
    
    std::string
    SequenceFromFiles::generateUserFriendlySequencePattern() const
    {
        if (isSingleFile()) {
            return _imp->filesMap.begin()->second.fileName();
            
        }
        std::string pattern = generateValidSequencePattern();
        removePath(pattern);
        return generateUserFriendlySequencePatternFromValidPattern(pattern);
        
    }
    
    std::string
    SequenceFromFiles::fileExtension() const
    {
        if (!empty()) {
            return _imp->filesMap.begin()->second.getExtension();
        } else {
            return std::string();
        }
        
    }
    
    std::string
    SequenceFromFiles::getPath() const
    {
        if (!empty()) {
            return _imp->filesMap.begin()->second.getPath();
        } else {
            return std::string();
        }
    }
    
    bool
    SequenceFromFiles::getSequenceOutOfFile(const std::string& absoluteFileName,SequenceFromFiles* sequence)
    {
        FileNameContent firstFile(absoluteFileName);
        sequence->tryInsertFile(firstFile);
        
        tinydir_dir dir;
        if (tinydir_open(&dir, firstFile.getPath().c_str()) == -1) {
            return false;
        }
        
        StringList allFiles;
        getFilesFromDir(dir, &allFiles);
        tinydir_close(&dir);
        
        for (StringList::iterator it = allFiles.begin(); it != allFiles.end(); ++it) {
            sequence->tryInsertFile(FileNameContent(firstFile.getPath() + *it));
        }
        return true;
    }
    
} // namespace SequenceParsing

