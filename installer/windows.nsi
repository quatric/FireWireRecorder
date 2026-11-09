; FireWireRecorder for Windows: the app, its bundled ffmpeg and the fwdv-capture command line tool.
;   makensis -DARCH=x86_64|x86|aarch64 -DVERSION=0.1.0 -DSRC=dist\x86_64-windows-gnu \
;            -DOUTFILE=FireWireRecorder-x86_64-windows-setup.exe installer/windows.nsi
; No driver: the camcorder uses Windows' own DV driver (MSDV).

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!ifndef ARCH | VERSION | SRC | OUTFILE
  !error "define ARCH, VERSION, SRC and OUTFILE"
!endif

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\FireWireRecorder"

Name "FireWireRecorder"
Caption "FireWireRecorder ${VERSION} Setup"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
!if "${ARCH}" == "x86"
  InstallDir "$PROGRAMFILES\FireWireRecorder"
!else
  InstallDir "$PROGRAMFILES64\FireWireRecorder"
!endif
InstallDirRegKey HKLM "Software\FireWireRecorder" "InstallDir"
BrandingText "FireWireRecorder ${VERSION}"

; (makensis works from this file's folder, so the path is relative to it)
!define MUI_ICON "../resources/windows/app.ico"
!define MUI_UNICON "../resources/windows/app.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\FireWireRecorder.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start FireWireRecorder"
!define MUI_COMPONENTSPAGE_SMALLDESC
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
!if "${ARCH}" == "x86_64"
  ${IfNot} ${IsNativeAMD64}
    MessageBox MB_ICONSTOP "This installer is for 64-bit Intel/AMD Windows.$\nUse the x86 installer on 32-bit Windows, or the aarch64 one on Windows on Arm."
    Abort
  ${EndIf}
  SetRegView 64
!else if "${ARCH}" == "aarch64"
  ${IfNot} ${IsNativeARM64}
    MessageBox MB_ICONSTOP "This installer is for Windows on Arm. Use the x86_64 installer on Intel/AMD PCs."
    Abort
  ${EndIf}
  SetRegView 64
!endif
FunctionEnd

Function un.onInit
!if "${ARCH}" != "x86"
  SetRegView 64
!endif
FunctionEnd

Section "FireWireRecorder" SecApp
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${SRC}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\FireWireRecorder.lnk" "$INSTDIR\FireWireRecorder.exe"
  WriteRegStr HKLM "Software\FireWireRecorder" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "FireWireRecorder"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "FireWireRecorder"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\FireWireRecorder.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\FireWireRecorder.lnk" "$INSTDIR\FireWireRecorder.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "FireWireRecorder, the fwdv-capture command line tool and ffmpeg for the preview, the sound and the MPEG-2/x264 recordings."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut to FireWireRecorder on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  Delete "$INSTDIR\FireWireRecorder.exe"
  Delete "$INSTDIR\ffmpeg.exe"
  Delete "$INSTDIR\fwdv-capture.exe"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\licenses\*.txt"
  RMDir "$INSTDIR\licenses"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\FireWireRecorder.lnk"
  Delete "$DESKTOP\FireWireRecorder.lnk"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\FireWireRecorder"
SectionEnd
