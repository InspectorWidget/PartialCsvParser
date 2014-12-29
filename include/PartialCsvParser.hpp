/**
 * \mainpage PartialCsvParser's reference manual
 *
 * This page is a reference manual of PartialCsvParser.
 *
 * @par
 *
 * To see installation, sample codes, and license, check the <a href="https://github.com/laysakura/partial_csv_parser">GitHub repository</a>.
 *
 * @author Sho Nakatani
 *
 * \license This project is distributed under public domain. See the <a href="https://github.com/laysakura/partial_csv_parser/UNLICENSE">UNLICENSE</a> file for more detailed explanation.
 */

#ifndef INCLUDE_PARTIALCSVPARSER_HPP_
#define INCLUDE_PARTIALCSVPARSER_HPP_

#include <vector>
#include <string>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

// Prevent default class methods
#define PREVENT_DEFAULT_CONSTRUCTOR(klass) \
  private: klass();
#define PREVENT_COPY_CONSTRUCTOR(klass) \
  private: klass(const klass&);
#define PREVENT_OBJECT_ASSIGNMENT(klass) \
  private: klass& operator=(const klass&);
#define PREVENT_CLASS_DEFAULT_METHODS(klass) \
  PREVENT_DEFAULT_CONSTRUCTOR(klass); \
  PREVENT_COPY_CONSTRUCTOR(klass); \
  PREVENT_OBJECT_ASSIGNMENT(klass); \


// Assertion also usable with Google Test
#ifdef PCP_GTEST
class PCPAssertionFailed : public std::runtime_error {
public:
  PCPAssertionFailed(const std::string &cause)
  : std::runtime_error(cause)
  {}
};

#define ASSERT(cond) \
  if (!(cond)) { \
    std::ostringstream ss; \
    ss << __FILE__ << ":" << __LINE__ << " (in " << __FUNCTION__ << "())" << " PCPAssertionFailed: " << #cond; \
    throw PCPAssertionFailed(ss.str()); \
  }

#else /* PCP_GTEST */
#include <cassert>

#define ASSERT(cond) \
  assert(cond);

#endif /* PCP_GTEST */


// Macros for error cases
#define STRERROR_THROW(err_class, msg) \
  { \
    throw err_class(std::string("Fatal from PartialCsvParser ") + msg + ": " + std::strerror(errno)); \
  }
#define PERROR_ABORT(msg) \
  { \
    std::perror((std::string("Fatal from PartialCsvParser ") + std::string(msg)).c_str()); \
    std::abort(); \
  }

namespace PCP {

/**
 * Runtime error.
 */
class PCPError : public std::runtime_error {
public:
  PCPError(const std::string &cause)
  : std::runtime_error(cause)
  {}
};

/**
 * Thrown when invalid CSV is parsed.
 *
 * CSV format is (weakly) defined in <a href="http://tools.ietf.org/html/rfc4180#section-2">RFC4180 Section2</a>
 *
 * PartialCsvParser throws this exception when one of the following conditions is satisfied.
 * @li A row has different number of columns from first row.
 */
class PCPCsvError : public PCPError {
public:
  PCPCsvError(const std::string &cause)
  : PCPError(cause)
  {}
};


// Utility functions
inline size_t _filesize(int opened_fd) throw(PCPError) {
  struct stat st;
  if (fstat(opened_fd, &st) != 0) STRERROR_THROW(PCPError, "while getting stat(2) of file");
  return st.st_size;
}

/**
 * Find a line including specified \p current_pos.
 * @param[in] text Original text to find a line from.
 * @param[in] text_length_byte Byte length of the original text.
 * @param[in] current_pos Current position taking 0 ~ (\p text_length - 1).
 * @param[in] line_terminator Character to terminate a line.
 * @param[out] line Pointer of start of the current line will be output.
 * @param[out] line_length_byte Byte length of current line (not including line terminator) will be output.
 *
   @verbatim
   \n aaaaaaaaaaa \n bbbbbbbbbbbbb \n cccccccccc \n
                     ^           ^  ^
                    (1)         (2)(3)
   @endverbatim
 *
 * If \p current_pos is at between (1) and (2), line will point at (1).
 * If \p current_pos is at (3), line will also point at (1).
 *
   @verbatim
   \n ddddddddd \n eeeeeeeeeee \0
                   ^            ^
                  (1)          (2)
   @endverbatim
 *
 * Consider null character.
 * If \p current_pos is at between (1) and (2), line will point at (1).
 */
inline void _get_current_line(
  const char * const text,
  size_t text_length_byte,
  size_t current_pos,
  char line_terminator,
  /* out */
  const char ** line,
  size_t * line_length_byte)
{
  ASSERT(text);
  ASSERT(text_length_byte >= 1);
  ASSERT(0 <= current_pos);
  ASSERT(current_pos <= text_length_byte - 1);

  // \n aaaaaaaaaaaa \n
  //    ^            ^
  //    start        end
  const char *line_start = text + current_pos, *line_end = text + current_pos;

  // search line_start
  while (line_start > text && *(line_start - 1) != line_terminator) --line_start;
  // search line_end
  while (line_end < text + text_length_byte && *line_end != line_terminator) ++line_end;

  *line = line_start;
  *line_length_byte = line_end - line_start;
}

inline std::vector<std::string> _split(const char * const str, size_t len, char delimiter) {
  ASSERT(str);
  ASSERT(len >= 0);

  std::vector<std::string> ret;  // NRVO optimization may prevent copy when returning this local variable.

  const char *p_beg = str, *p_end = str;
  while (p_end - str < len) {
    // come to delimiter
    if (*p_end == delimiter) {
      ret.push_back(std::string(p_beg, p_end - p_beg));
      p_beg = p_end + 1;
    }
    ++p_end;
  }
  // come to end of str
  ret.push_back(std::string(p_beg, p_end - p_beg));
  return ret;
}


class CsvConfig;
class PartialCsvParser;

class CsvConfig {
public:
  /**
   * Constructor.
   * @param filepath Path to CSV file to read.
   * @param has_header_line If CSV file has header at first line, set true.
   * @param field_terminator Character to separate columns. For UTF-8 compatibility, only 0 ~ 127 are allowed.
   * @param line_terminator Character to separate rows. For UTF-8 compatibility, only 0 ~ 127 are allowed.
   */
  CsvConfig(
    const char * const filepath,
    bool has_header_line = true,
    char field_terminator = ',',
    char line_terminator = '\n')
  throw(PCPError)
  : filepath(filepath), has_header_line(has_header_line),
    field_terminator(field_terminator), line_terminator(line_terminator),
    n_columns(0)
  {
    // UTF-8 compatibility
    ASSERT(0 <= field_terminator); ASSERT(field_terminator <= 127);
    ASSERT(0 <= line_terminator); ASSERT(line_terminator <= 127);

    if ((fd = open(filepath, O_RDONLY)) == -1)
      STRERROR_THROW(PCPError, std::string("while open ") + filepath);
    csv_size = _filesize(fd);
    if ((csv_text = static_cast<const char *>(mmap(NULL, csv_size, PROT_READ, MAP_PRIVATE, fd, 0))) == (void*)-1)
      STRERROR_THROW(PCPError, std::string("while mmap ") + filepath);

    // parse first line to calculate n_columns
    const char * line = 0;
    size_t line_length = 0;
    _get_current_line(csv_text, csv_size, 0, line_terminator, &line, &line_length);
    std::vector<std::string> columns = _split(line, line_length, field_terminator);
    n_columns = columns.size();

    // set headers if exist
    if (has_header_line) {
      header_length = line_length;
      headers = columns;
    }
  }

  ~CsvConfig() {
    if (munmap((void*)csv_text, csv_size) != 0) PERROR_ABORT("while munmap");
    if (close(fd) != 0) PERROR_ABORT("while closing file descriptor");
  }

  /**
   * Return the size of CSV file.
   */
  inline size_t filesize() const { return csv_size; }

  /**
   * Return the pointer of CSV content.
   */
  inline const char * const content() const { return csv_text; }

  /**
   * Return the number of columns in first line.
   */
  inline size_t get_n_columns() const { return n_columns; }

  /**
   * Return the offset where CSV body (excluding header line) starts from.
   */
  inline size_t body_offset() const {
    if (!has_header_line) return 0;
    return header_length + 1;
  }

  /**
   * Return header string array.
   * \p has_header_line flag must be set true in constructor.
   */
  inline std::vector<std::string> get_headers() const {
    ASSERT(has_header_line);
    return headers;
  }

  /**
   * Return a character to separate columns.
   */
  inline const char get_field_terminator() const { return field_terminator; }
  /**
   * Return a character to separate rows.
   */
  inline const char get_line_terminator() const { return line_terminator; }

private:
  const char * const filepath;
  const bool has_header_line;
  const char field_terminator;
  const char line_terminator;

  int fd;
  size_t csv_size;
  const char * csv_text;

  std::vector<std::string> headers;
  size_t header_length;

  size_t n_columns;

  PREVENT_CLASS_DEFAULT_METHODS(CsvConfig);
};


/**
 * Set of parameters passed to PartialCsvParser::PartialCsvParser().
 */
typedef struct partial_csv_t {
  CsvConfig & csv_config;
  size_t parse_from;
  size_t parse_to;
} partial_csv_t;


class PartialCsvParser {
public:

  /**
   * Constructor.
   * @param csv_config Instance of CsvConfig.
   * @param parse_from CSV file's <em>approximate</em> offset to start parsing. Must be no less than CsvConfig::body_offset().
   *   \p parse_from = PARSE_FROM_BODY_BEGINNING has the same meaning with \p parse_from = CsvConfig::body_offset().
   * @param parse_to CSV file's <em>approximate</em> offset to stop parsing. Must be no less than \p parse_from and less than CsvConfig::filesize().
   *   \p parse_to = PARSE_TO_FILE_END has the same meaning with \p parse_to = CsvConfig::filesize() - 1.
   *
   * In order to fully parse CSV lines without overlaps, \p parse_from and \p parse_to are interpreted with the following strategy.
   *
   * <-------> means range from parse_from to parse_to.
   *
     @verbatim
     (beginning of CSV)  aaaaaaaaaaaaaaaa \0
                         <---><-----><-->
                          (1)   (2)   (3)
     @endverbatim
   *
   * In this severe edge case, only (1), <b>who covers the beginning of line</b>, parses line "aaaaaaaaaaaaaaaa" to prevent overlap.
   *
   *
     @verbatim
     (beginning of CSV)  aaaaaaaaaaaaa \n bbbbbbbbbbb \0
                         <---><-------->  <--------->
                          (1)    (2)          (3)
     @endverbatim
   *
   * In this case, (1) parses "aaaaaaaaaaaaa", (2) parses no line, and (3) parses "bbbbbbbbbbb".
   *
     @verbatim
     (beginning of CSV)  aaaaaaaaaaaaa \n bbbbbbbbbbb \0
                         <---><-----------><-------->
                          (1)      (2)         (3)
     @endverbatim
   *
   * In this case, (1) parses "aaaaaaaaaaaaa", (2) parses "bbbbbbbbbbb", and (3) parses no line.
   *
   * In short, <b>partial parser who covers the beginning of a line parses the line</b>.
   */
  PartialCsvParser(
    CsvConfig & csv_config,
    size_t parse_from = PARSE_FROM_BODY_BEGINNING,
    size_t parse_to = PARSE_TO_FILE_END)
  : csv_config(csv_config), parse_from(parse_from), parse_to(parse_to)
  {
    if (parse_from == PARSE_FROM_BODY_BEGINNING) this->parse_from = csv_config.body_offset();
    if (parse_to == PARSE_TO_FILE_END) this->parse_to = csv_config.filesize() - 1;
    cur_pos = this->parse_from;
    ASSERT(csv_config.body_offset() <= this->parse_from);
    ASSERT(this->parse_to < csv_config.filesize());
    // Do not assert this->parse_from <= this->parse_to.
    // This case occurs when CSV file only has header, has no body.
    // Even in such case, get_row() can be safely called, which returns empty vector.
  }

  ~PartialCsvParser() {}

  /**
   * Returns an array of parsed columns.
   * Parses only around [\p parse_from, \p parse_to) specified in constructor is parsed.
   * @return Array of columns if line to parse remains. Otherwise, empty vector is returned. Check by \p retval.empty().
   */
  inline std::vector<std::string> get_row() throw(PCPCsvError) {
    while (cur_pos <= parse_to) {
      const char * line;
      size_t line_length;
      _get_current_line(csv_config.content(), csv_config.filesize(), cur_pos, csv_config.get_line_terminator(), &line, &line_length);

      // cur_pos exactly is the beginning of current line.
      //
      // (\n or beginning of CSV file)  aaaaaaaaaaaaaa \n
      //                                <----...
      //                                cur_pos
      //
      // Parse "aaaaaaaaaaaaaa" and move cur_pos to the beginning of the next line.
      if (csv_config.content() + cur_pos == line) {
        cur_pos += line_length + 1;  // +1 is from line_delimitor

        const std::vector<std::string> & columns = _split(line, line_length, csv_config.get_field_terminator());
        if (columns.size() != csv_config.get_n_columns()) {
          std::ostringstream ss; \
          ss << "The following line has " << columns.size() << " columns, while the first line has " << csv_config.get_n_columns() << " columns." << std::endl << std::string(line, line_length);
          throw PCPCsvError(ss.str());
        }
        return columns;
      }

      // parse_to is at the same line with cur_pos.
      //
      // (\n or beginning of CSV file)  aaaaaaaaaaaaaa \n
      //                                    <---------->
      //                                    cur_pos    parse_to
      if (csv_config.content() + parse_to < line + line_length + 1)  // +1 is from line_delimitor
        return std::vector<std::string>(0);

      // parse_to is beyond the same line with cur_pos.
      //
      // (\n or beginning of CSV file)  aaaaaaaaaaaaaa \n bbbbbbbbbb
      //                                    <--------------...
      //                                    cur_pos
      //
      // Move cur_pos to the beginning of the next line.
      if (csv_config.content() + parse_to >= line + line_length + 1)  // +1 is from line_delimitor
        cur_pos = (line - csv_config.content()) + line_length + 1;  // +1 is from line_delimitor
    }
    return std::vector<std::string>(0);
  }

private:
  static const size_t PARSE_FROM_BODY_BEGINNING = -1;
  static const size_t PARSE_TO_FILE_END = -1;

  CsvConfig & csv_config;
  size_t parse_from, parse_to;
  size_t cur_pos;

  PREVENT_CLASS_DEFAULT_METHODS(PartialCsvParser);
};


}

#endif /* INCLUDE_PARTIALCSVPARSER_HPP_ */
