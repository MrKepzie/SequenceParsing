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

#ifndef __IO__SequenceParser__
#define __IO__SequenceParser__

#include <map>
#include <vector>
#include <list>
#include <string>
#include <memory>

typedef std::vector<std::string> StringList ;
namespace SequenceParsing {



/**
     * @brief A class representing the content of a filename.
     * Initialize it passing it a real filename and it will initialize the data structures
     * depending on the filename content. This class is used by the file dialog to find sequences.
     **/
struct FileNameContentPrivate;
class FileNameContent {

public:

    explicit FileNameContent(const std::string& absoluteFilename);

    FileNameContent(const FileNameContent& other);

    ~FileNameContent();

    void operator=(const FileNameContent& other);

    /**
         * @brief Returns the file path, e.g: /Users/Lala/Pictures/ with the trailing separator.
         **/
    const std::string& getPath() const;

    /**
         * @brief Returns the filename without its path.
         **/
    const std::string& fileName() const;

    /**
         * @brief Returns the absolute filename as it was given in the constructor arguments.
         **/
    const std::string& absoluteFileName() const;

    /**
         * @brief Returns the file extension
         **/
    const std::string& getExtension() const;


    /**
         * @brief Returns the file pattern found in the filename with hash characters style for frame number (i.e: ###)
         * A few things about this pattern:
         * - The file path is not prepended to the file name.
         * - Each hash tag corresponding to a potential frame number will be indexed. For example,
         * The following filename:
         * my80sequence001.jpg would be transformed in my##1sequence###2.jpg.
         * This is because there may be several numbers existing in the filename, and we have
         * no clue given just this filename what actually corresponds to the frame number.
         * Nb: this pattern is not an absolute path.
         **/
    const std::string& getFilePattern(int numHashes) const;

    /**
     * @brief Returns how many '0' are prepended to the frame number of the file
     **/
    int getNumPrependingZeroes() const;

    /**
         * @brief Expands the string returned by getFilePattern to a valid pattern.
         * In order to make it a valid pattern we remove all hash tags indexes and expand
         * the tags that do not correspond to index.
         * The pattern will have its file path prepended so it is absolute.
         **/
    void generatePatternWithFrameNumberAtIndex(int index,int numHashes,std::string* pattern) const;

    /**
         * @brief If the filename is composed of several numbers (e.g: file08_001.png),
         * this functions returns the number at index as a string that will be stored in numberString.
         * If Index is greater than the number of numbers in the filename or if this filename doesn't
         * contain any number, this function returns false.
         **/
    bool getNumberByIndex(int index,std::string* numberString) const;

    /**
         * @brief Returns the number of potential frame numbers the filename is composed of.
         * e.g: file08_001.png would return 2.
       **/
    int getPotentialFrameNumbersCount() const;

    /**
         * @brief Given the pattern of this file, it tries to match the other file name to this
         * pattern.
         * @param numberIndexToVary [out] In case the pattern contains several numbers (@see getNumberByIndex)
         * this value will be fed the appropriate number index that should be used for frame number.
         * For example, if this filename is myfile001_000.jpg and the other file is myfile001_001.jpg
         * numberIndexesToVary would have a single element (1) as the frame number string indentied in that case is the last number.
         * Another example: my00file000_000.jpg and my00file001_001.jpg would produce 1 and 2 in numberIndexesToVary.
         * @returns True if it identified 'other' as belonging to the same sequence, false otherwise.
         * Note that this function will return false if this and other are exactly the same.
         **/
    bool matchesPattern(const FileNameContent& other,int* numberIndexToVary) const;


private:
    std::auto_ptr<FileNameContentPrivate> _imp; // PImpl
};

/**
     * @brief Removes from str any path it may have and returns the path.
     * The file doesn't have to necessarily exist on the disk.
     * For example: /Users/Lala/Pictures/mySequence001.jpg would transform 'filename' in
     * mySequence001.jpg and would return /Users/Lala/Pictures/
     **/
std::string removePath(std::string& filename);



///map: < time, map < view_index, file name > >
///Explanation: for each frame number, there may be multiple views, each mapped to a filename.
///If there'are 2 views, index 0 is considered to be the left view and index 1 is considered to be the right view.
///If there'are multiple views, the index is corresponding to the view
///This type is used when retrieving an existing file sequence out of a pattern.
typedef std::map<int,std::map<int,std::string> > SequenceFromPattern;
/**
     * @brief Given a pattern string, returns a string list with all the existing file names
     * matching the mattern.
     * The rules of the pattern are:
     *
     * 1) Hashes character ('#') are counted as one digit. One digit account for extra
     * 0 padding that may exist prepending the actual number. Therefore
     * /Users/Lala/Pictures/mySequence###.jpg will accept the following filenames:
     * /Users/Lala/Pictures/mySequence001.jpg
     * /Users/Lala/Pictures/mySequence100.jpg
     * /Users/Lala/Pictures/mySequence1000.jpg
     * But not /Users/Lala/Pictures/mySequence01.jpg because it has less digits than the hashes characters specified.
     * Neither /Users/Lala/Pictures/mySequence01000.jpg because it has extra padding and more digits than hashes characters specified.
     *
     * 2) The same as hashes characters can be achieved by specifying the option %0<paddingCharactersCount>d
     * Ex: /Users/Lala/Pictures/mySequence###.jpg can be achieved the same way with /Users/Lala/Pictures/mySequence%03d.jpg
     * You can also use %d which will just match any number.
     *
     * 3) %v will be replaced automatically by 'l' or 'r' to search for a matching filename.
     *  For example /Users/Lala/Pictures/mySequence###_%v.jpg would accept:
     * /Users/Lala/Pictures/mySequence001_l.jpg and /Users/Lala/Pictures/mySequence001_r.jpg
     *
     * %V would achieve exactly the same but would be replaced by 'left' or 'right' instead.
     * For multi-view it would expend to 'view2','view3', etc...
     *
     * @returns True if it could extract a valid sequence from the pattern (even with 1 filename in it). False if the pattern
     * is not valid.
     **/
bool filesListFromPattern(const std::string& pattern,SequenceParsing::SequenceFromPattern* sequence);


/**
     * @brief Transforms a sequence parsed from a pattern to a absolute file names list. If
     * onlyViewIndex is greater or equal to 0 it will append to the string list only file names
     * whose view index matches  'onlyViewIndex'. Otherwise, it is equal to -1, all files
     * will be appended to the return value, regardless of the view index.
     **/
StringList sequenceFromPatternToFilesList(const SequenceParsing::SequenceFromPattern& sequence,
                                          int onlyViewIndex = -1);

/**
     * @brief Generates a filename out of a pattern
     * @see filesListFromPattern
     **/
std::string generateFileNameFromPattern(const std::string& pattern,int frameNumber,int viewNumber);

/**
     * @struct Used to gather file together that seem to belong to the same sequence.
     * This is used for example in the sequence dialog. It aims to produce a pattern
     * out of a series of file.
     **/
struct SequenceFromFilesPrivate;
class SequenceFromFiles {


public:

    explicit SequenceFromFiles(bool enableSizeEstimation = false);

    /**
         * @brief Initializes a sequence with a first file.
         * @param enableSizeEstimation If true, it will try to evaluate the cumulated sizes
         * of all files in the sequence.
         **/
    SequenceFromFiles(const FileNameContent& firstFile, bool enableSizeEstimation);

    SequenceFromFiles(const SequenceFromFiles& other);

    ~SequenceFromFiles();

    /**
         * @brief Given the absoluteFileName, this function returns the sequence of all the files that
         * seem to match the pattern of that file.
         * @param sequence[out] Contains at least 1 file once this function has returned.
         * @brief Returns true on success, false otherwise. This function returns false if you have provided
         * a wrong filename for example.
         **/
    static bool getSequenceOutOfFile(const std::string& absoluteFileName,SequenceFromFiles* sequence);

    void operator=(const SequenceFromFiles& other) const;

    ///Tries to insert a file in the sequence and returns true if it succeeded,
    ///indicating that the file matches the sequence or it is already contained in this sequence.
    ///@param checkPath If true then this function will check if the path of the file is the same as
    ///the sequence
    bool tryInsertFile(const FileNameContent& file,bool checkPath = true);

    ///Returns true if this sequence contains the given file.
    bool contains(const std::string& absoluteFileName) const;

    ///is the sequence empty ?
    bool empty() const;

    ///get the number of files
    int count() const;

    ///get the file extension (e.g: "jpg")
    std::string fileExtension() const;

    ///Returns the path with a trailing separator.
    std::string getPath() const;

    ///true if there's only a single file in this sequence
    bool isSingleFile() const;

    /// INT_MIN if there's no index
    int getFirstFrame() const;

    /// INT_MAX if there's no index
    int getLastFrame() const;

    ///all the frame indexes. Empty if this is not a sequence.
    const std::map<int,FileNameContent>& getFrameIndexes() const;

    ///Returns the total cumulated size of all files in the sequence.
    ///If enableSizeEstimation is false, it will return 0.
    unsigned long long getEstimatedTotalSize() const;

    ///Generates a pattern from this sequence.
    ///Normally calling filesListFromPattern on the result of this function
    ///should find the exact same files as getFilesList() would return.
    std::string generateValidSequencePattern() const;

    ////Generates a string from this sequence so the user can have a global
    ////understanding of the content of the sequence.
    ////If the sequence contains only a single file, it will be the exact same name
    ////the filename without the path.
    std::string generateUserFriendlySequencePattern() const;

private:
    std::auto_ptr<SequenceFromFilesPrivate> _imp; // PImpl
};



} //namespace SequenceParsing

#endif /* defined(__IO__SequenceParser__) */
