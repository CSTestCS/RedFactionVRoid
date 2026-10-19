# Turns a text file into a C++ raw string literal: cmake -DIN=... -DOUT=... -DNAME=... -P embed_text.cmake
file(READ "${IN}" CONTENT)
file(WRITE "${OUT}" "static const char ${NAME}[] = R\"RFVEMBED(${CONTENT})RFVEMBED\";\n")
