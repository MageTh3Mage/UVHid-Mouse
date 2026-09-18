@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /std:c++17 /O2 /EHsc /W4 /DUNICODE /D_UNICODE test.cpp /Fe:test.exe /link hid.lib setupapi.lib
