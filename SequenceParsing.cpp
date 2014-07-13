/*
 SequenceParser is a small class that helps reading a sequence of images within a directory.


 Copyright (C) 2013 INRIA
 Author Alexandre Gauthier-Foichat alexandre.gauthier-foichat@inria.fr

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 Neither the name of the {organization} nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 INRIA
 Domaine de Voluceau
 Rocquencourt - B.P. 105
 78153 Le Chesnay Cedex - France

 */
#include "SequenceParsing.h"

#include <cassert>
#include <cmath>
#include <climits>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <locale>
#include <istream>
#include <algorithm>


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

namespace  {


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
                                                      std::vector<std::pair<std::string,int> >* variablesByOrder) {
    int i = 0;
    bool inPrintfLikeArg = false;
    int printfLikeArgIndex = 0;
    std::string commonPart;
    std::string variable;
    int commonCharactersFound = 0;
    bool previousCharIsSharp = false;
    while (i < (int)patternUnPathedWithoutExt.size()) {
        const char& c = patternUnPathedWithoutExt.at(i);
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
                next = patternUnPathedWithoutExt.at(i + 1);
            }
            char prev = '\0';
            if (i > 0) {
                prev = patternUnPathedWithoutExt.at(i -1);
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
        ++i;
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


// templated version of my_equal so it could work with both char and wchar_t
template<typename charT>
struct my_equal {
    my_equal( const std::locale& loc ) : loc_(loc) {}
    bool operator()(charT ch1, charT ch2) {
        return std::toupper(ch1, loc_) == std::toupper(ch2, loc_);
    }
private:
    const std::locale& loc_;
};

// find substring (case insensitive)
template<typename T>
size_t ci_find_substr( const T& str1, const T& str2, const std::locale& loc = std::locale() )
{
    typename T::const_iterator it = std::search( str1.begin(), str1.end(),
                                                 str2.begin(), str2.end(), my_equal<typename T::value_type>(loc) );
    if ( it != str1.end() ) return it - str1.begin();
    else return std::string::npos; // not found
}

static size_t findStr(const std::string& from,const std::string& toSearch,int pos, bool caseSensitive = false)
{
    if (caseSensitive) {
        return from.find(toSearch,pos);
    } else {
        return ci_find_substr<std::string>(from, toSearch);
    }
}


static bool startsWith(const std::string& str,const std::string& prefix,bool caseSensitive = false)
{
    return findStr(str,prefix,0,caseSensitive) == 0;
}

static bool endsWith(const std::string &str, const std::string &suffix)
{
    return str.size() >= suffix.size() &&
            str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static void removeAllOccurences(std::string& str,const std::string& toRemove,bool caseSensitive = false)
{
    if (str.size()) {
        size_t i = 0;
        while ((i = findStr(str, toRemove, i,caseSensitive)) != std::string::npos) {
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

static std::string removeFileExtension(std::string& filename) {
    int i = filename.size() -1;
    std::string extension;
    while(i>=0 && filename.at(i) != '.') {
        extension.insert(0,1,filename.at(i));
        --i;
    }
    filename = filename.substr(0,i);
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

static bool matchesHashTag(int sharpCount,const std::string& filename,
                           size_t startingPos,const std::locale& loc,
                           size_t *endPos,int* frameNumber)
{
    std::string variable;
    size_t variableIt = startingPos;
    while (variableIt < filename.size() && std::isdigit(filename.at(variableIt),loc)) {
        variable.push_back(filename.at(variableIt));
         ++variableIt;
    }
    *endPos = variableIt;

    if ((int)variable.size() < sharpCount) {
        return false;
    }

    int prepending0s = 0;
    int i = 0;
    while (i < (int)variable.size()) {
        if (variable.at(i) != '0') {
            break;
        } else {
            ++prepending0s;
        }
        ++i;
    }

    ///extra padding on numbers bigger than the hash chars count are not allowed.
    if ((int)variable.size() > sharpCount && prepending0s > 0) {
        return false;
    }

    *frameNumber = stringToInt(variable);
    return true;

}

static bool matchesPrintfLikeSyntax(int digitsCount,const std::string& filename,
                                    size_t startingPos,const std::locale& loc,
                                    size_t *endPos,int* frameNumber) {

    std::string variable;
    size_t variableIt = startingPos;
    while (variableIt < filename.size() && std::isdigit(filename.at(variableIt),loc)) {
        variable.push_back(filename.at(variableIt));
         ++variableIt;
    }
    *endPos = variableIt;

    if ((int)variable.size() < digitsCount) {
        return false;
    }

    int prepending0s = 0;
    int i = 0;
    while (i < (int)variable.size()) {
        if (variable.at(i) != '0') {
            break;
        } else {
            ++prepending0s;
        }
        ++i;
    }

    ///extra padding on numbers bigger than the hash chars count are not allowed.
    if ((int)variable.size() > digitsCount && prepending0s > 0) {
        return false;
    }

    *frameNumber = stringToInt(variable);
    return true;
}

static bool matchesView(bool longView,const std::string& filename,
                                    size_t startingPos,const std::locale& loc,
                        size_t *endPos,int* viewNumber) {

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
            size_t it = 4;
            std::string viewNoStr;
            while (it < mid.size() && std::isdigit(mid.at(it),loc)) {
                viewNoStr.push_back(mid.at(it));
                ++it;
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
            size_t it = 4;
            std::string viewNoStr;
            while (it < mid.size() && std::isdigit(mid.at(it),loc)) {
                viewNoStr.push_back(mid.at(it));
                ++it;
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
                              const std::locale& loc,
                              int* frameNumber,int* viewNumber)
{

    ///If the frame number is found twice or more, this is to verify if they are identical
    bool wasFrameNumberSet = false;

    ///If the view number is found twice or more, this is to verify if they are identical
    bool wasViewNumberSet = false;

    ///Default view number and frame number
    *viewNumber = 0;
    *frameNumber = -1;

    ///Iterators on the pattern and the filename
    size_t filenameIt = 0;
    size_t patternIt = 0;

    ///make a copy of the filename from which we remove the file extension
    std::string filenameCpy = filename;
    std::string fileExt = removeFileExtension(filenameCpy);

    ///Extensions not matching, exit.
    if (fileExt != patternExtension) {
        return false;
    }

    ///Iterating while not at end of either the pattern or the filename
    while (filenameIt < filenameCpy.size() && patternIt < pattern.size())
    {
        ///the count of '#' characters found
        int sharpCount = 0;

        ///Actually start counting the #
        size_t sharpIt = patternIt;
        std::string variable;
        while (sharpIt < pattern.size() && pattern.at(sharpIt) == '#') {
            ++sharpIt;
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
        if (pattern.at(patternIt) == '%')
        {
            ///We found the '%' digit, start at the character right after to
            ///find digits
            size_t printfIt = patternIt + 1;
            std::string digitStr;
            while (printfIt < pattern.size() && std::isdigit(pattern.at(printfIt),loc))
            {
                digitStr.push_back(pattern.at(printfIt));
                ++printfIt;
                ++printfLikeVariableSize;
            }

            ///they are no more digit after the '%', check if this is correctly terminating by a 'd' character.
            /// We also treat the view %v and %V cases here
            if (printfIt < pattern.size() && std::tolower(pattern.at(printfIt),loc) == 'd') {
                foundPrintFLikeSyntax = true;
                printfDigitCount = stringToInt(digitStr);
            } else if (printfIt < pattern.size() && pattern.at(printfIt) == 'V') {
                foundLongView = true;
            } else if (printfIt < pattern.size() && pattern.at(printfIt) == 'v') {
                foundShortView = true;
            }
        }


        if (sharpCount > 0) ///If we found #
        {
            ///There cannot be another variable!
            assert(!foundPrintFLikeSyntax && !foundLongView && !foundShortView);
            size_t endHashTag;
            int fNumber;

            ///check if the filename matches the number of hashes
            if (!matchesHashTag(sharpCount,filenameCpy,filenameIt,loc,&endHashTag,&fNumber)) {
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

            size_t endPrintfLike;
            int fNumber;
            ///check if the filename matches the %d syntax
            if (!matchesPrintfLikeSyntax(printfDigitCount,filenameCpy,filenameIt,loc,&endPrintfLike,&fNumber)) {
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
            if (!matchesView(true,filenameCpy,filenameIt,loc,&endVar,&vNumber)) {
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
            if (!matchesView(false,filenameCpy,filenameIt,loc,&endVar,&vNumber)) {
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

            ///we found nothing, just compare the characters without case sensitivity
            if (std::tolower(pattern.at(patternIt),loc) != std::tolower(filenameCpy.at(filenameIt),loc)) {
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
    bool hasSingleNumber;
    std::string generatedPattern;

    FileNameContentPrivate()
        : orderedElements()
        , absoluteFileName()
        , filePath()
        , filename()
        , extension()
        , hasSingleNumber(false)
        , generatedPattern()
    {
    }

    void parse(const std::string& absoluteFileName);
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

FileNameContent::~FileNameContent() {
    delete _imp;
}

void FileNameContent::operator=(const FileNameContent& other) {
    _imp->orderedElements = other._imp->orderedElements;
    _imp->absoluteFileName = other._imp->absoluteFileName;
    _imp->filename = other._imp->filename;
    _imp->filePath = other._imp->filePath;
    _imp->extension = other._imp->extension;
    _imp->hasSingleNumber = other._imp->hasSingleNumber;
    _imp->generatedPattern = other._imp->generatedPattern;
}

void FileNameContentPrivate::parse(const std::string& absoluteFileName) {
    this->absoluteFileName = absoluteFileName;
    filename = absoluteFileName;
    filePath = removePath(filename);

    int i = 0;
    std::string lastNumberStr;
    std::string lastTextPart;
    while (i < (int)filename.size()) {
        const char& c = filename.at(i);
        if (std::isdigit(c,std::locale())) {
            lastNumberStr += c;
            if (!lastTextPart.empty()) {
                orderedElements.push_back(FileNameElement(lastTextPart,FileNameElement::TEXT));
                lastTextPart.clear();
            }
        } else {
            if (!lastNumberStr.empty()) {
                orderedElements.push_back(FileNameElement(lastNumberStr,FileNameElement::FRAME_NUMBER));
                if (!hasSingleNumber) {
                    hasSingleNumber = true;
                } else {
                    hasSingleNumber = false;
                }
                lastNumberStr.clear();
            }

            lastTextPart.push_back(c);
        }
        ++i;
    }

    if (!lastNumberStr.empty()) {
        orderedElements.push_back(FileNameElement(lastNumberStr,FileNameElement::FRAME_NUMBER));
        if (!hasSingleNumber) {
            hasSingleNumber = true;
        } else {
            hasSingleNumber = false;
        }
        lastNumberStr.clear();
    }
    if (!lastTextPart.empty()) {
        orderedElements.push_back(FileNameElement(lastTextPart,FileNameElement::TEXT));
        lastTextPart.clear();
    }



    size_t lastDotPos = filename.find_last_of('.');
    if (lastDotPos != std::string::npos) {
        int j = filename.size() - 1;
        while (j > 0 && filename.at(j) != '.') {
            extension.insert(0,1,filename.at(j));
            --j;
        }
    }

}

StringList FileNameContent::getAllTextElements() const {
    StringList ret;
    for (unsigned int i = 0; i < _imp->orderedElements.size(); ++i) {
        if (_imp->orderedElements[i].type == FileNameElement::TEXT) {
            ret.push_back(_imp->orderedElements[i].data);
        }
    }
    return ret;
}

/**
     * @brief Returns the file path, e.g: /Users/Lala/Pictures/ with the trailing separator.
     **/
const std::string& FileNameContent::getPath() const {
    return _imp->filePath;
}

/**
     * @brief Returns the filename without its path.
     **/
const std::string& FileNameContent::fileName() const {
    return _imp->filename;
}

/**
     * @brief Returns the absolute filename as it was given in the constructor arguments.
     **/
const std::string& FileNameContent::absoluteFileName() const {
    return _imp->absoluteFileName;
}

const std::string& FileNameContent::getExtension() const {
    return _imp->extension;
}


/**
     * @brief Returns true if a single number was found in the filename.
     **/
bool FileNameContent::hasSingleNumber() const {
    return _imp->hasSingleNumber;
}

/**
     * @brief Returns true if the filename is composed only of digits.
     **/
bool FileNameContent::isFileNameComposedOnlyOfDigits() const {
    if ((_imp->orderedElements.size() == 1 || _imp->orderedElements.size() == 2)
            && _imp->orderedElements[0].type == FileNameElement::FRAME_NUMBER) {
        return true;
    } else {
        return false;
    }
}

/**
     * @brief Returns the file pattern found in the filename with hash characters style for frame number (i.e: ###)
     **/
const std::string& FileNameContent::getFilePattern() const {
    if (_imp->generatedPattern.empty()) {
        ///now build the generated pattern with the ordered elements.
        int numberIndex = 0;
        for (unsigned int j = 0; j < _imp->orderedElements.size(); ++j) {
            const FileNameElement& e = _imp->orderedElements[j];
            switch (e.type) {
                case FileNameElement::TEXT:
                    _imp->generatedPattern.append(e.data);
                    break;
                case FileNameElement::FRAME_NUMBER:
                {
                    std::string hashStr;
                    int c = 0;
                    while (c < (int)e.data.size()) {
                        hashStr.push_back('#');
                        ++c;
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
bool FileNameContent::getNumberByIndex(int index,std::string* numberString) const {

    int numbersElementsIndex = 0;
    for (unsigned int i = 0; i < _imp->orderedElements.size(); ++i) {
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
    for (unsigned int i = 0; i < _imp->orderedElements.size(); ++i) {
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
bool FileNameContent::matchesPattern(const FileNameContent& other,std::vector<int>* numberIndexesToVary) const {

    const std::vector<FileNameElement>& otherElements = other._imp->orderedElements;
    if (otherElements.size() != _imp->orderedElements.size()) {
        return false;
    }


    ///We only consider the last potential frame number
    ///
    int nbVaryingFrameNumbers = 0;
    int frameNumberIndexStringIndex = -1;

    int numbersCount = 0;
    for (unsigned int i = 0; i < _imp->orderedElements.size(); ++i) {
        if (_imp->orderedElements[i].type != otherElements[i].type) {
            return false;
        }
        if (_imp->orderedElements[i].type == FileNameElement::FRAME_NUMBER) {
            if (_imp->orderedElements[i].data != otherElements[i].data) {
                ///if one frame number string is longer than the other, make sure it is because the represented number
                ///is bigger and not because there's extra padding
                /// For example 10000 couldve been produced with ## only and is valid, and 01 would also produce be ##.
                /// On the other hand 010000 could never have been produced with ## hence it is not valid.

                bool valid = true;
                ///if they have different sizes, if one of them starts with a 0 its over.
                if (_imp->orderedElements[i].data.size() != otherElements[i].data.size()) {

                    if (_imp->orderedElements[i].data.size() > otherElements[i].data.size()) {
                        if (otherElements[i].data.at(0) == '0' && otherElements[i].data.size() > 1) {
                            valid = false;
                        } else {
                            int k = 0;
                            int diff = std::abs((int)_imp->orderedElements[i].data.size()  - (int)otherElements[i].data.size());
                            while (k < (int)_imp->orderedElements[i].data.size() && k < diff) {
                                if (_imp->orderedElements[i].data.at(k) == '0') {
                                    valid = false;
                                }
                                break;
                                ++k;
                            }
                        }
                    } else {
                        if (_imp->orderedElements[i].data.at(0) == '0' && _imp->orderedElements[i].data.size() > 1) {
                            valid = false;
                        } else {
                            int k = 0;
                            int diff = std::abs((int)_imp->orderedElements[i].data.size()  - (int)otherElements[i].data.size());
                            while (k < (int)otherElements[i].data.size() && k < diff) {
                                if (otherElements[i].data.at(k) == '0') {
                                    valid = false;
                                }
                                break;
                                ++k;
                            }
                        }
                    }

                }
                if (valid) {
                   frameNumberIndexStringIndex = numbersCount;
                   ++nbVaryingFrameNumbers;
                }

            }
            ++numbersCount;
        } else if (_imp->orderedElements[i].type == FileNameElement::TEXT && _imp->orderedElements[i].data != otherElements[i].data) {
            return false;
        }
    }
    ///strings are identical
    /// we only accept files with 1 varying number
    if (frameNumberIndexStringIndex == -1 || nbVaryingFrameNumbers != 1) {
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
//    for (unsigned int i = 0; i < potentialFrameNumbers.size(); ++i) {
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
//    for (unsigned int i = 0; i < minIndexes.size(); ++i) {
//        numberIndexesToVary->push_back(potentialFrameNumbers[minIndexes[i]].first);
//    }
    numberIndexesToVary->push_back(frameNumberIndexStringIndex);
    return true;

}

void FileNameContent::generatePatternWithFrameNumberAtIndexes(const std::vector<int>& indexes,std::string* pattern) const {
    int numbersCount = 0;
    size_t lastNumberPos = 0;
    std::string indexedPattern = getFilePattern();
    for (unsigned int i = 0; i < _imp->orderedElements.size(); ++i) {
        if (_imp->orderedElements[i].type == FileNameElement::FRAME_NUMBER) {
            lastNumberPos = findStr(indexedPattern, "#", lastNumberPos,true);
            assert(lastNumberPos != std::string::npos);

            int endTagPos = lastNumberPos;
            while (endTagPos < (int)indexedPattern.size() && indexedPattern.at(endTagPos) == '#') {
                ++endTagPos;
            }

            ///assert that the end of the tag is composed of  a digit
            if (endTagPos < (int)indexedPattern.size()) {
                assert(std::isdigit(indexedPattern.at(endTagPos),std::locale()));
            }

            bool isNumberAFrameNumber = false;
            for (unsigned int j = 0; j < indexes.size(); ++j) {
                if (indexes[j] == numbersCount) {
                    isNumberAFrameNumber = true;
                    break;
                }
            }
            if (!isNumberAFrameNumber) {
                ///if this is not the number we're interested in to keep the ###, just expand the variable
                ///replace the whole tag with the original data
                indexedPattern.replace(lastNumberPos, endTagPos - lastNumberPos + 1, _imp->orderedElements[i].data);
            } else {
                ///remove the index of the tag and keep the tag.
                if (endTagPos < (int)indexedPattern.size()) {
                    indexedPattern.erase(endTagPos, 1);
                }
            }
            lastNumberPos = endTagPos;

            ++numbersCount;
        }
    }

    *pattern = getPath() + indexedPattern;
}


std::string removePath(std::string& filename) {

    ///find the last separator
    size_t pos = filename.find_last_of('/');
    if (pos == std::string::npos) {
        //try out \\ char
        pos = filename.find_last_of('\\');
    }
    if(pos == std::string::npos) {
        ///couldn't find a path
        return "";
    }
    std::string path = filename.substr(0,pos+1); // + 1 to include the trailing separator
    removeAllOccurences(filename, path,true);
    return path;
}


static bool filesListFromPattern_internal(const std::string& pattern,SequenceParsing::SequenceFromPattern* sequence)
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

    std::locale loc;

    for (int i = 0; i < (int)files.size(); ++i) {
        int frameNumber;
        int viewNumber;
        if (matchesPattern_v2(files.at(i),patternUnPathed,patternExtension,loc,&frameNumber,&viewNumber)) {
            SequenceFromPattern::iterator it = sequence->find(frameNumber);
            std::string absoluteFileName = patternPath + files.at(i);
            if (it != sequence->end()) {
                std::pair<std::map<int,std::string>::iterator,bool> ret =
                        it->second.insert(std::make_pair(viewNumber,absoluteFileName));
                if (!ret.second) {
                    std::cerr << "There was an issue populating the file sequence. Several files with the same frame number"
                                 " have the same view index." << std::endl;
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

bool filesListFromPattern(const std::string& pattern,SequenceParsing::SequenceFromPattern* sequence) {
    return filesListFromPattern_internal(pattern,sequence);
}

StringList sequenceFromPatternToFilesList(const SequenceParsing::SequenceFromPattern& sequence,int onlyViewIndex ) {
    StringList ret;
    for (SequenceParsing::SequenceFromPattern::const_iterator it = sequence.begin(); it!=sequence.end(); ++it) {
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

std::string generateFileNameFromPattern(const std::string& pattern,int frameNumber,int viewNumber) {
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
    for (unsigned int i = 0; i < variablesByOrder.size(); ++i) {
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
                viewNumberStr = std::string("view") + stringFromInt(viewNumber);
            }

            output.replace(lastVariablePos,variable.size(), viewNumberStr);
        } else if (variable.find("%V") != std::string::npos) {
            std::string viewNumberStr;
            if (viewNumber == 0) {
                viewNumberStr = "left";
            } else if (viewNumber == 1) {
                viewNumberStr = "right";
            } else {
                viewNumberStr = std::string("view") + stringFromInt(viewNumber);
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
    /// the parsed files that have matching content with respect to variables.
    std::vector < FileNameContent > sequence;

    ///a list with all the files in the sequence, with their absolute file names.
    StringList filesList;

    ///all the files mapped to their index
    std::map<int,std::string> filesMap;

    /// The index of the frame number string in case there're several numbers in a filename.
    std::vector<int> frameNumberStringIndexes;

    unsigned long long totalSize;

    bool sizeEstimationEnabled;

    SequenceFromFilesPrivate(bool enableSizeEstimation)
        : sequence()
        , filesList()
        , filesMap()
        , frameNumberStringIndexes()
        , totalSize(0)
        , sizeEstimationEnabled(enableSizeEstimation)
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
    _imp->sequence.push_back(firstFile);
    _imp->filesList.push_back(firstFile.absoluteFileName());
    if (enableSizeEstimation) {
        std::ifstream file(firstFile.absoluteFileName().c_str(), std::ios::binary | std::ios::ate);
        _imp->totalSize += file.tellg();
    }
}

SequenceFromFiles::~SequenceFromFiles() {
    delete _imp;
}

SequenceFromFiles::SequenceFromFiles(const SequenceFromFiles& other)
    : _imp(new SequenceFromFilesPrivate(false))
{
    *this = other;
}

void SequenceFromFiles::operator=(const SequenceFromFiles& other) const {
    _imp->sequence = other._imp->sequence;
    _imp->filesList = other._imp->filesList;
    _imp->filesMap = other._imp->filesMap;
    _imp->frameNumberStringIndexes = other._imp->frameNumberStringIndexes;
    _imp->totalSize = other._imp->totalSize;
    _imp->sizeEstimationEnabled = other._imp->sizeEstimationEnabled;
}

bool SequenceFromFiles::tryInsertFile(const FileNameContent& file) {

    if (_imp->filesList.empty()) {
        _imp->sequence.push_back(file);
        _imp->filesList.push_back(file.absoluteFileName());
        if (_imp->sizeEstimationEnabled) {
            std::ifstream f(file.absoluteFileName().c_str(), std::ios::binary | std::ios::ate);
            _imp->totalSize += f.tellg();
        }
        return true;
    }

    if (file.getPath() != _imp->sequence[0].getPath()) {
        return false;
    }

    std::vector<int> frameNumberIndexes;
    bool insert = false;
    const  FileNameContent& firstFileContent = _imp->sequence[0];
    if (file.matchesPattern(firstFileContent, &frameNumberIndexes)) {

        if (std::find(_imp->filesList.begin(),_imp->filesList.end(),file.absoluteFileName()) != _imp->filesList.end()) {
            return false;
        }

        if (_imp->frameNumberStringIndexes.empty()) {
            ///this is the second file we add to the sequence, we can now
            ///determine where is the frame number string placed.
            _imp->frameNumberStringIndexes = frameNumberIndexes;
            insert = true;

            ///insert the first frame number in the frameIndexes.
            std::string firstFrameNumberStr;

            for (unsigned int i = 0; i < frameNumberIndexes.size(); ++i) {
                std::string frameNumberStr;
                bool ok = firstFileContent.getNumberByIndex(_imp->frameNumberStringIndexes[i], &frameNumberStr);
                if (ok && firstFrameNumberStr.empty()) {
                    _imp->filesMap.insert(std::make_pair(stringToInt(frameNumberStr),firstFileContent.absoluteFileName()));
                    firstFrameNumberStr = frameNumberStr;
                } else if (!firstFrameNumberStr.empty() && stringToInt(frameNumberStr) != stringToInt(firstFrameNumberStr)) {
                    return false;
                }
            }


        } else if(frameNumberIndexes == _imp->frameNumberStringIndexes) {
            insert = true;
        }
        if (insert) {

            std::string firstFrameNumberStr;

            for (unsigned int i = 0; i < frameNumberIndexes.size(); ++i) {
                std::string frameNumberStr;
                bool ok = file.getNumberByIndex(_imp->frameNumberStringIndexes[i], &frameNumberStr);
                if (ok && firstFrameNumberStr.empty()) {
                    _imp->sequence.push_back(file);
                    _imp->filesList.push_back(file.absoluteFileName());
                    _imp->filesMap.insert(std::make_pair(stringToInt(frameNumberStr),file.absoluteFileName()));
                    if (_imp->sizeEstimationEnabled) {
                        std::ifstream f(file.absoluteFileName().c_str(), std::ios::binary | std::ios::ate);
                        _imp->totalSize += f.tellg();
                    }

                    firstFrameNumberStr = frameNumberStr;
                } else if (!firstFrameNumberStr.empty() && stringToInt(frameNumberStr) != stringToInt(firstFrameNumberStr)) {
                    return false;
                }
            }
        }
    }
    return insert;
}

bool SequenceFromFiles::contains(const std::string& absoluteFileName) const {
    return std::find(_imp->filesList.begin(),_imp->filesList.end(),absoluteFileName) != _imp->filesList.end();
}

bool SequenceFromFiles::empty() const {
    return _imp->filesList.empty();
}

int SequenceFromFiles::count() const {
    return (int)_imp->filesList.size();
}

bool SequenceFromFiles::isSingleFile() const {
    return _imp->sequence.size() == 1;
}

int SequenceFromFiles::getFirstFrame() const {
    if (_imp->filesMap.empty()) {
        return INT_MIN;
    } else {
        return _imp->filesMap.begin()->first;
    }
}

int SequenceFromFiles::getLastFrame() const {
    if (_imp->filesMap.empty()) {
        return INT_MAX;
    } else {
        std::map<int,std::string>::const_iterator it = _imp->filesMap.end();
        --it;
        return it->first;
    }
}

const std::map<int,std::string>& SequenceFromFiles::getFrameIndexes() const {
    return _imp->filesMap;
}

const StringList& SequenceFromFiles::getFilesList() const {
    return _imp->filesList;
}

unsigned long long SequenceFromFiles::getEstimatedTotalSize() const {
    return _imp->totalSize;
}

std::string SequenceFromFiles::generateValidSequencePattern() const
{
    if (empty()) {
        return "";
    }
    if (isSingleFile()) {
        return _imp->sequence[0].absoluteFileName();
    }
    assert(_imp->filesMap.size() >= 2);
    std::string firstFramePattern ;
    _imp->sequence[0].generatePatternWithFrameNumberAtIndexes(_imp->frameNumberStringIndexes, &firstFramePattern);
    return firstFramePattern;
}

std::string SequenceFromFiles::generateUserFriendlySequencePattern() const {
    if (isSingleFile()) {
        return _imp->sequence[0].fileName();
    }
    std::string pattern = generateValidSequencePattern();
    removePath(pattern);

    std::vector< std::pair<int,int> > chunks;
    int first = getFirstFrame();
    while(first <= getLastFrame()){

        int breakCounter = 0;
        while (!(_imp->isInSequence(first)) && breakCounter < NATRON_DIALOG_MAX_SEQUENCES_HOLE) {
            ++first;
            ++breakCounter;
        }

        if (breakCounter >= NATRON_DIALOG_MAX_SEQUENCES_HOLE) {
            break;
        }

        chunks.push_back(std::make_pair(first, getLastFrame()));
        int next = first + 1;
        int prev = first;
        int count = 1;
        while((next <= getLastFrame())
              && _imp->isInSequence(next)
              && (next == prev + 1) ){
            prev = next;
            ++next;
            ++count;
        }
        --next;
        chunks.back().second = next;
        first += count;
    }

    if (chunks.size() == 1) {
        pattern += ' ';
        pattern += stringFromInt(chunks[0].first);
        pattern += '-';
        pattern += stringFromInt(chunks[0].second);
    } else {
        pattern.append(" ( ");
        for(unsigned int i = 0 ; i < chunks.size() ; ++i) {
            if(chunks[i].first != chunks[i].second){
                pattern += ' ';
                pattern += stringFromInt(chunks[i].first);
                pattern += '-';
                pattern += stringFromInt(chunks[i].second);
            }else{
                pattern += ' ';
                pattern += stringFromInt(chunks[i].first);
            }
            if(i < chunks.size() -1) pattern.append(" /");
        }
        pattern.append(" ) ");
    }
    return pattern;
}

std::string SequenceFromFiles::fileExtension() const {
    if (!empty()) {
        return _imp->sequence[0].getExtension();
    } else {
        return "";
    }
}

std::string SequenceFromFiles::getPath() const {
    if (!empty()) {
        return _imp->sequence[0].getPath();
    } else {
        return "";
    }
}

bool SequenceFromFiles::getSequenceOutOfFile(const std::string& absoluteFileName,SequenceFromFiles* sequence)
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

    for (StringList::iterator it = allFiles.begin(); it!=allFiles.end(); ++it) {
        sequence->tryInsertFile(FileNameContent(firstFile.getPath() + *it));
    }
    return true;
}

} // namespace SequenceParsing

