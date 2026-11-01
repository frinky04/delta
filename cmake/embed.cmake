# Turns a binary file into a C++ byte array.  Usage:
#   cmake -DIN=file -DOUT=file.cpp -DNAME=symbol -P embed.cmake
file(READ ${IN} hex HEX)
if(MANUAL)
  # The screenshots belong to the repository README, not the offline manual.
  file(READ ${IN} text)
  string(REGEX REPLACE "<!-- screenshots:start -->.*<!-- screenshots:end -->\r?\n?" "" text "${text}")
  string(HEX "${text}" hex)
endif()
string(LENGTH "${hex}" hex_len)
math(EXPR size "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUT}
  "#include <cstddef>\n"
  "extern const unsigned char ${NAME}[] = {${bytes}};\n"
  "extern const std::size_t ${NAME}_size = ${size};\n")
