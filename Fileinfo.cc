/*
   copyright 20016-2017 Paul Dreik (earlier Paul Sundvall)
   Distributed under GPL v 2.0 or later, at your option.
   See LICENSE for further details.
*/
#include "Fileinfo.hh"
#include "config.h"

#include <cerrno> //for errno
#include <cstring>
#include <fstream>    //for file reading
#include <iostream>   //for cout etc
#include <sys/stat.h> //for file info
#include <unistd.h>   //for unlink etc.

#include "Checksum.hh" //checksum calculation
#include "UndoableUnlink.hh"

using std::cerr;
using std::endl;

int
Fileinfo::fillwithbytes(enum readtobuffermode filltype,
                        enum readtobuffermode lasttype)
{

  // Decide if we are going to read from file or not.
  // If file is short, first bytes might be ALL bytes!
  if (lasttype != readtobuffermode::NOT_DEFINED) {
    if (this->size() <= static_cast<filesizetype>(m_somebytes.size())) {
      // pointless to read - all bytes in the file are in the field
      // m_somebytes, or checksum is calculated!
      //      cout<<"Skipped reading from file because lasttype="<<lasttype
      //	  <<" and size="<<size()<<endl;
      return 0;
    }
  }

  // set memory to zero
  m_somebytes.fill('\0');

  std::fstream f1;
  f1.open(m_filename.c_str(), std::ios_base::in);
  if (!f1.is_open()) {
    std::cerr << "fillwithbytes.cc: Could not open file \"" << m_filename
              << "\"" << std::endl;
    return -1;
  }

  int checksumtype = Checksum::NOTSET;
  // read some bytes
  switch (filltype) {
    case readtobuffermode::READ_FIRST_BYTES:
      // read at start of file
      f1.read(m_somebytes.data(), SomeByteSize);
      break;
    case readtobuffermode::READ_LAST_BYTES:
      // read at end of file
      f1.seekg(-SomeByteSize, std::ios_base::end);
      f1.read(m_somebytes.data(), SomeByteSize);
      break;
    case readtobuffermode::CREATE_MD5_CHECKSUM:
      checksumtype = Checksum::MD5;
      break;
    case readtobuffermode::CREATE_SHA1_CHECKSUM:
      checksumtype = Checksum::SHA1;
      break;
    case readtobuffermode::CREATE_SHA256_CHECKSUM:
      checksumtype = Checksum::SHA256;
      break;
    default:
      std::cerr << "does not know how to do that filltype:"
                << static_cast<long>(filltype) << std::endl;
  }

  if (checksumtype != Checksum::NOTSET) {
    Checksum chk;
    if (chk.init(checksumtype))
      std::cerr << "error in checksum init" << std::endl;
    char buffer[4096];
    while (f1) {
      f1.read(buffer, sizeof(buffer));
      chk.update(f1.gcount(), buffer);
    }

    // store the result of the checksum calculation in somebytes
    int digestlength = chk.getDigestLength();
    if (digestlength <= 0 ||
        digestlength >= static_cast<int>(m_somebytes.size())) {
      std::cerr << "wrong answer from getDigestLength! FIXME" << std::endl;
    }
    if (chk.printToBuffer(m_somebytes.data(), m_somebytes.size())) {
      std::cerr << "failed writing digest to buffer!!" << std::endl;
    }
  }

  f1.close();
  return 0;
}

bool
Fileinfo::readfileinfo()
{
  struct stat info;
  m_info.is_file = false;
  m_info.is_directory = false;

  int res;
  do {
    res = stat(m_filename.c_str(), &info);
  } while (res < 0 && errno == EINTR);

  if (res < 0) {
    m_info.stat_size = 0;
    m_info.stat_ino = 0;
    m_info.stat_dev = 0;
    std::cerr << "readfileinfo.cc:Something went wrong when reading file "
                 "info from \""
              << m_filename << "\" :" << strerror(errno) << std::endl;
    return false;
  }

  // only keep the relevant information
  m_info.stat_size = info.st_size;
  m_info.stat_ino = info.st_ino;
  m_info.stat_dev = info.st_dev;

  m_info.is_file = S_ISREG(info.st_mode) ? true : false;
  m_info.is_directory = S_ISDIR(info.st_mode) ? true : false;
  return true;
}



std::string
Fileinfo::getduptypestring(const Fileinfo& A)
{

  switch (A.getduptype()) {
    case DUPTYPE_UNKNOWN:
      return "DUPTYPE_UNKNOWN";
    case DUPTYPE_FIRST_OCCURRENCE:
      return "DUPTYPE_FIRST_OCCURRENCE";
    case DUPTYPE_WITHIN_SAME_TREE:
      return "DUPTYPE_WITHIN_SAME_TREE";
    case DUPTYPE_OUTSIDE_TREE:
      return "DUPTYPE_OUTSIDE_TREE";
    default:
      std::cerr << "error. does not know that one" << std::endl;
  }
  return "error-error";
}

// constructor
Fileinfo::Fileinfostat::Fileinfostat()
{
  stat_size = 99999;
  stat_ino = 99999;
  stat_dev = 99999;
  is_file = false;
  is_directory = false;
}

int
Fileinfo::deletefile()
{
  return unlink(name().c_str());
}

namespace {
int
simplifyPath(std::string& path)
{
  // replace a/./b with a/b
  do {
    const auto pos = path.find("/./");
    if (pos == std::string::npos) {
      break;
    }
    path.replace(pos, 3, "/");
  } while (true);

  // replace repeated slashes
  do {
    const auto pos = path.find("//");
    if (pos == std::string::npos) {
      break;
    }
    path.replace(pos, 2, "/");
  } while (true);

  // getting rid of /../ is difficult to get correct because of symlinks.
  // do not do it.
  return 0;
}

// prepares target, so that location can point to target in
// the best way possible
int
makeAbsolute(std::string& target)
{
  // if target is not absolute, let us make it absolute
  if (target.length() > 0 && target.at(0) == '/') {
    // absolute. do nothing.
  } else {
    // not absolute. make it absolute.

    // yes, this is possible to do with dynamically allocated memory,
    // but it is not portable then (and more work).
    const size_t buflength = 256;
    char buf[buflength];
    if (buf != getcwd(buf, buflength)) {
      std::cerr << "failed to get current working directory" << std::endl;
      return -1;
    }
    target = std::string(buf) + std::string("/") + target;
  }
  return 0;
}

/**
 * helper for transactional operation on a file. It will move the file to a
 * temporary, then invoke f with the filename as argument, then delete the
 * temporary. In case of failure at any point, it will do it's best to restore
 * the temporary back to the filename.
 * @param filename
 * @param f
 * @return zero on success.
 */
template<typename Func>
int
transactional_operation(const std::string& filename, const Func& f)
{
  // move the file to a temporary.
  UndoableUnlink restorer(filename);

  // did the move to a temporary go ok?
  if (!restorer.file_is_moved()) {
    return 1;
  }

  // yes, apply whatever the caller wanted.
  const int ret = f(filename);

  if (0 != ret) {
    // operation failed! rollback time (made by restorer at scope exit)
    return ret;
  }

  // operation succeeded, go ahead and unlink the temporary.
  if (0 != restorer.unlink()) {
    return 1;
  }

  return 0;
}
} // anon. namespace

// makes a symlink that points to A
int
Fileinfo::makesymlink(const Fileinfo& A)
{
  const int retval =
    transactional_operation(name(), [&](const std::string& filename) {
      // The tricky thing is that the path must be correct, as seen from
      // the directory where *this is. Making the path absolute solves this
      // problem. Doing string manipulations to find how to make the path
      // relative is prone to error because directories can be symlinks.
      std::string target = A.name();
      makeAbsolute(target);
      // clean up the path, so it does not contain sequences "/./" or "//"
      simplifyPath(target);
      return symlink(target.c_str(), filename.c_str());
    });

  if (retval) {
    cerr << "failed to make symlink " << name() << " to " << A.name() << endl;
  }
  return retval;
}

// makes a hard link that points to A
int
Fileinfo::makehardlink(const Fileinfo& A)
{
  return transactional_operation(name(), [&](const std::string& filename) {
    // make a hardlink.
    const int retval = link(A.name().c_str(), filename.c_str());
    if (retval) {
      cerr << "failed to make hardlink " << filename << " to " << A.name()
           << endl;
    }
    return retval;
  });
}

int
Fileinfo::static_deletefile(Fileinfo& A, const Fileinfo& /*B*/)
{
  return A.deletefile();
}

int
Fileinfo::static_makesymlink(Fileinfo& A, const Fileinfo& B)
{
  return A.makesymlink(B);
}

int
Fileinfo::static_makehardlink(Fileinfo& A, const Fileinfo& B)
{
  return A.makehardlink(B);
}

bool
Fileinfo::compareonbytes(const Fileinfo& a, const Fileinfo& b)
{
  const int retval =
    std::memcmp(a.getbyteptr(), b.getbyteptr(), a.m_somebytes.size());
  return retval < 0;
}

bool
Fileinfo::equalbytes(const Fileinfo& a, const Fileinfo& b)
{
  const int retval =
    std::memcmp(a.getbyteptr(), b.getbyteptr(), a.m_somebytes.size());
  return retval == 0;
}

bool
Fileinfo::compareonsizeandfirstbytes(const Fileinfo& a, const Fileinfo& b)
{
  if (a.size() > b.size())
    return true;
  if (a.size() < b.size())
    return false;
  // must be equal. compare on bytes.
  return compareonbytes(a, b);
}

bool
Fileinfo::equalsize(const Fileinfo& a, const Fileinfo& b)
{
  return a.size() == b.size();
}
