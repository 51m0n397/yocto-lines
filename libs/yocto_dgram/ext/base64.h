// https://stackoverflow.com/questions/180947/base64-decode-snippet-in-c

#ifndef _BASE64_H_
#define _BASE64_H_

#include <string>
#include <vector>

std::string base64_encode(unsigned char const* buf, unsigned int bufLen);
std::vector<unsigned char> base64_decode(std::string const&);

#endif