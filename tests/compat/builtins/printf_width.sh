# width, precision, left alignment and zero padding
printf '[%5s][%-5s][%.2s]\n' ab ab abcdef
printf '[%05d][%-4d][%+d]\n' 42 7 3
printf '[%*s]\n' 6 hi
printf '[%8.3f][%.0f][%e]\n' 3.14159 2.5 1234.5
