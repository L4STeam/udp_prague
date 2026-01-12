#ifndef JSON_WRITER_H
#define JSON_WRITER_H

#include <cstdint>
#include <fstream>
#include <string>

// Simple JSON lines file writer
struct json_writer {
  std::string buf;
  std::string file;
  bool first = true;

  int init(const char *filename, bool append = true) {
    if (!filename || !*filename)
      return -1;
    file = filename;
    if (!append)
      std::ofstream(file, std::ios::trunc).close();
    return 0;
  }

  void reset() {
    buf.clear();
    buf.push_back('{');
    first = true;
  }

  void sep() {
    if (!first)
      buf.push_back(',');
    first = false;
  }

  /* Field functions */
  void field(const char *k, const std::string &v) {
    sep();
    buf += "\"";
    buf += k;
    buf += "\":\"";
    buf += v;
    buf += "\"";
  }

  void field(const char *k, uint64_t v) {
    sep();
    buf += "\"";
    buf += k;
    buf += "\":\"";
    buf += std::to_string(v);
    buf += "\"";
  }

  void field(const char *k, int32_t v) {
    sep();
    buf += "\"";
    buf += k;
    buf += "\":\"";
    buf += std::to_string(v);
    buf += "\"";
  }

  void field(const char *k, float v) {
    sep();
    buf += "\"";
    buf += k;
    buf += "\":\"";
    buf += std::to_string(v);
    buf += "\"";
  }

  /* Finalize I/O */
  void finalize() { buf.push_back('}'); }

  int dump() {
    std::ofstream out(file, std::ios::app | std::ios::binary);
    if (!out)
      return -1;
    out << buf << '\n';
    return 0;
  }
};

#endif //! JSON_WRITER_H
