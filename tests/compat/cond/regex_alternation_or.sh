# [[ $OS =~ A || $OS =~ B ]] (flutter update_dart_sdk style)
OS=MSYS_NT-10.0
if [[ $OS =~ MINGW.* || $OS =~ CYGWIN.* || $OS =~ MSYS.* ]]; then echo windows; fi
