@echo off
rem MKWSK.CMD - build WINSOCK.DLL, the Stage C 16-bit Windows Sockets 1.1
rem library for WfW 3.11 over the PMDOS.386 bridge.  Module name WINSOCK,
rem the standard Winsock 1.1 ordinals, so a program linked against the
rem stock winsock import library binds to this DLL at run time.  Compiled
rem -zu (DS != SS in a DLL) and -bd; each export is __loadds so it loads
rem DGROUP (the loader fixes up the push ds/pop/nop prolog).
rem
rem Run from the project root (S:\CPP\PM-DOS) or from here; needs the
rem Windows 16-bit headers on INCLUDE.  Kept out of BUILD.CMD for the same
rem reason as the VxD (CMD\PMDOS386): Step 7 builds with the Windows
rem 16-bit tools, not the PM-DOS flat set BUILD.CMD uses.
setlocal
set INCLUDE=C:\WATCOM\H\WIN;C:\WATCOM\H
cd /d %~dp0
wasm -q -zq wskglue.asm || goto fail
wcc -q -bt=windows -bd -ml -s -zu winsock.c || goto fail
wlink system windows_dll name ..\..\..\BIN\WINSOCK.DLL ^
  option modname=WINSOCK option description 'PM-DOS Windows Sockets 1.1 (WfW bridge)' ^
  file winsock.obj file wskglue.obj ^
  export WSAStartup.115 export WSACleanup.116 ^
  export accept.1 export bind.2 export closesocket.3 export connect.4 ^
  export getpeername.5 export getsockname.6 export getsockopt.7 ^
  export htonl.8 export htons.9 export inet_addr.10 export inet_ntoa.11 ^
  export ioctlsocket.12 export listen.13 export ntohl.14 export ntohs.15 ^
  export recv.16 export recvfrom.17 export select.18 export send.19 ^
  export sendto.20 export setsockopt.21 export shutdown.22 export socket.23 ^
  export gethostbyaddr.51 export gethostbyname.52 export getprotobyname.53 ^
  export getprotobynumber.54 export getservbyname.55 export getservbyport.56 ^
  export gethostname.57 ^
  export WSAAsyncSelect.101 export WSAAsyncGetHostByAddr.102 ^
  export WSAAsyncGetHostByName.103 export WSAAsyncGetProtoByNumber.104 ^
  export WSAAsyncGetProtoByName.105 export WSAAsyncGetServByPort.106 ^
  export WSAAsyncGetServByName.107 export WSACancelAsyncRequest.108 ^
  export WSASetBlockingHook.109 export WSAUnhookBlockingHook.110 ^
  export WSAGetLastError.111 export WSASetLastError.112 ^
  export WSACancelBlockingCall.113 export WSAIsBlocking.114 ^
  export __WSAFDIsSet.151 export WEP.500 ^
  export ws_ontimer.200 || goto fail
echo MKWSK-OK
exit /b 0
:fail
echo MKWSK-FAIL errorlevel %errorlevel%
exit /b 1
