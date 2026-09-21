; SPDX-License-Identifier: GPL-2.0-or-later
; Copyright (C) 2026 The Isotone authors
;
; The Windows installer (stage 6).
;
; This script copies files and calls isotone-devicetool. Every change to HKLM
; and to the ProgramData ACL is a devicetool command with a --dry-run and tests
; behind it (machine-install, machine-uninstall), so none of that logic is
; written here, where it could not be tested.
;
; There is no page for choosing outputs. The app's first run does that
; (ui/qml/FirstRun.qml) and Settings, Outputs does it again later.
;
; Build:
;   cmake --install build-ui --prefix <stage>
;   makensis /DSTAGE=<stage> /DVERSION=0.1.0 windows/setup/isotone.nsi

Unicode true
ManifestDPIAware true

!ifndef STAGE
  !error "STAGE is the staged install tree: makensis /DSTAGE=..."
!endif
!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef OUTFILE
  !define OUTFILE "isotone-${VERSION}-setup.exe"
!endif

!define APPNAME "Isotone"
!define PUBLISHER "The Isotone authors"
!define ARPKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Isotone"
!define RUNKEY "Software\Microsoft\Windows\CurrentVersion\Run"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${APPNAME}" "InstallDir"
; HKLM, the COM class and the ProgramData ACL all need it. There is no
; per-user install: audiodg runs as LocalService and cannot read a user profile.
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${APPNAME} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "Copyright (C) 2026 ${PUBLISHER}. GPL v2 or later."

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WinVer.nsh"
!include "nsDialogs.nsh"

; ---------------------------------------------------------------- appearance

!define MUI_ICON "${__FILEDIR__}\..\..\ui\res\isotone.ico"
!define MUI_UNICON "${__FILEDIR__}\..\..\ui\res\isotone.ico"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "${__FILEDIR__}\header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "${__FILEDIR__}\header.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${__FILEDIR__}\welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${__FILEDIR__}\welcome.bmp"
!define MUI_ABORTWARNING

; ---------------------------------------------------------------------- text
;
; Short on purpose (owner, 2026-09-19). The one thing that has to be said is
; where presets live, and it is said once.

!define MUI_WELCOMEPAGE_TITLE "${APPNAME} Setup"
!define MUI_WELCOMEPAGE_TEXT "A system-wide parametric EQ with a modern UI.$\r$\n$\r$\nInstallation includes the app and the engine."

!define MUI_LICENSEPAGE_TEXT_TOP " "
!define MUI_LICENSEPAGE_TEXT_BOTTOM "Click I Agree to continue."
!define MUI_LICENSEPAGE_BUTTON "I Agree"

!define MUI_DIRECTORYPAGE_TEXT_TOP "Program Files only: Windows runs the engine as LOCAL SERVICE, which cannot read a user profile."

!define MUI_FINISHPAGE_TITLE "${APPNAME} is installed"
!define MUI_FINISHPAGE_TEXT "Saved presets are stored in %APPDATA%\${APPNAME}."
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Open ${APPNAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION RunAsUser
; The second checkbox, borrowed from the readme slot: launch at sign-in, the
; same HKCU value Settings, General writes (ui/backend/startup_registration.h).
!define MUI_FINISHPAGE_SHOWREADME
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Start when I sign in"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION EnableAutostart
!define MUI_FINISHPAGE_NOREBOOTSUPPORT

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${__FILEDIR__}\..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!define MUI_UNCONFIRMPAGE_TEXT_TOP "Are you sure you want to uninstall ${APPNAME}?"
!define MUI_PAGE_CUSTOMFUNCTION_SHOW un.ConfirmShow
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE un.ConfirmLeave
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------- init
;
; makensis builds a 32-bit installer, so every HKLM write is redirected into
; WOW6432Node unless the view is set. Isotone is 64-bit only: the APO is loaded
; by a 64-bit audiodg and the app is a 64-bit build, so its keys belong in the
; 64-bit view. Measured on the first real install, which put the whole
; Add/Remove Programs entry in the wrong place (2026-09-19).
;
; SetShellVarContext all: this is a machine-wide install, so the Start menu
; entry is for every user, not for whoever happened to approve the elevation.

Function .onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

; -------------------------------------------------------------------- shared

Var DeleteData

; Isotone must not be running while its files are replaced or removed. Windows
; keeps a running image open for reading and deleting only, so its exe cannot be
; opened for writing while the app is up (measured 2026-09-21). Without this an
; install over a running one stops at NSIS's "error opening file for writing",
; and an uninstall leaves isotone.exe and the whole Qt runtime in $INSTDIR:
; RMDir /REBOOTOK does not take a directory that still has files in it, so
; nothing is scheduled to remove them either.
;
; FileOpen "a" rather than a process list: it asks exactly the question that
; matters, needs no plugin, and is right however the app was started. A file
; that is not there yet, which is every first install, is not running.
!macro NotRunning un
Function ${un}CheckNotRunning
  ${Do}
    ClearErrors
    ${IfNot} ${FileExists} "$INSTDIR\isotone.exe"
      Return
    ${EndIf}
    FileOpen $0 "$INSTDIR\isotone.exe" a
    ${IfNot} ${Errors}
      FileClose $0
      Return
    ${EndIf}
    ${If} ${Silent}
      DetailPrint "Isotone is running; close it and run this again."
      Abort "Isotone is running."
    ${EndIf}
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
      "Isotone is running.$\r$\n$\r$\nQuit it from its tray icon, then click Retry." IDRETRY +2
    Abort "Isotone is running."
  ${Loop}
FunctionEnd
!macroend
!insertmacro NotRunning ""
!insertmacro NotRunning "un."

; devicetool prints one JSON object; on failure its exit code says why. The
; whole object goes into the details log either way, so a failed install can be
; read afterwards rather than guessed at.
!macro Devicetool args fail
  nsExec::ExecToLog '"$INSTDIR\isotone-devicetool.exe" ${args}'
  Pop $0
  ${If} $0 != 0
    DetailPrint "failed with exit code $0"
    MessageBox MB_ICONSTOP "${fail}$\r$\n$\r$\nisotone-devicetool exited with $0. The details window has what it printed."
    Abort "${fail}"
  ${EndIf}
!macroend

; ------------------------------------------------------------------- install

Section "Isotone" SecMain
  SectionIn RO

  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "Isotone needs Windows 10 or later."
    Abort
  ${EndIf}

  ; An upgrade over a running app cannot replace its files.
  Call CheckNotRunning

  SetOutPath "$INSTDIR"
  ; The whole staged tree: the app, the engine, the devicetool and the Qt
  ; runtime windeployqt put there (ui/CMakeLists.txt).
  File /r "${STAGE}\*.*"

  DetailPrint "Registering the audio engine"
  !insertmacro Devicetool 'machine-install --dll "$INSTDIR\IsoAPO.dll"' \
                          "Setup could not register the audio engine."

  WriteRegStr HKLM "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\${APPNAME}" "Version" "${VERSION}"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Add or remove programs.
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegStr   HKLM "${ARPKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr   HKLM "${ARPKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr   HKLM "${ARPKEY}" "DisplayIcon" "$INSTDIR\isotone.exe"
  WriteRegStr   HKLM "${ARPKEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr   HKLM "${ARPKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${ARPKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr   HKLM "${ARPKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegStr   HKLM "${ARPKEY}" "URLInfoAbout" "https://github.com/jackwangxyw/Isotone"
  WriteRegDWORD HKLM "${ARPKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${ARPKEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${ARPKEY}" "EstimatedSize" "$0"

  CreateShortcut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\isotone.exe"
SectionEnd

; The app must not inherit the installer's elevated token: it runs as the user,
; writes %APPDATA% as the user, and asks for elevation itself when an output
; changes. Explorer is unelevated, so starting it through Explorer drops the
; token without needing a plugin.
Function RunAsUser
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\isotone.exe"'
FunctionEnd

; The same value Settings, General writes, so the toggle there agrees with it.
; Written to the HKCU of whoever approved the elevation: for the usual case,
; an administrator elevating their own session, that is the right hive. Under
; over-the-shoulder elevation it lands in the administrator's, and the user's
; Settings simply shows the toggle off.
Function EnableAutostart
  WriteRegStr HKCU "${RUNKEY}" "${APPNAME}" '"$INSTDIR\isotone.exe" --tray'
FunctionEnd

; ----------------------------------------------------------------- uninstall

Function un.ConfirmShow
  ; MUI's confirm page with one checkbox added, which is the whole page
  ; (the approved mockup, docs/design/logo/out/setup-6-uninstall.png).
  StrCpy $DeleteData "0"
  ; The confirm page's own inner dialog, so the checkbox sits with its text
  ; rather than floating over the outer window.
  FindWindow $0 "#32770" "" $HWNDPARENT
  System::Call 'user32::CreateWindowEx(i 0, t "BUTTON", t "Also delete saved settings and presets",       i ${DEFAULT_STYLES}|${BS_AUTOCHECKBOX}|${WS_TABSTOP}, i 0, i 40, i 300, i 16,       p $0, p 0, p 0, p 0) p .R9'
  SendMessage $R9 ${WM_SETFONT} "$(^Font)" 1
  ShowWindow $R9 ${SW_SHOW}
FunctionEnd

Function un.ConfirmLeave
  ${NSD_GetState} $R9 $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $DeleteData "1"
  ${Else}
    StrCpy $DeleteData "0"
  ${EndIf}
FunctionEnd

Section "Uninstall"
  ; A running app keeps its own files, and its tray icon would go on offering an
  ; engine that is no longer registered.
  Call un.CheckNotRunning

  ; machine-uninstall takes IsoAPO off every output it is on, then unregisters
  ; the class, in that order: unregistering while a slot still names the CLSID
  ; leaves that output with no audio. It removes DisableProtectedAudioDG only
  ; if this install set it and Equalizer APO is not there to need it.
  ${If} $DeleteData == "1"
    !insertmacro Devicetool 'machine-uninstall --remove-data' \
                            "Setup could not remove the audio engine."
  ${Else}
    !insertmacro Devicetool 'machine-uninstall' \
                            "Setup could not remove the audio engine."
  ${EndIf}

  DetailPrint "Restarting Windows audio"
  nsExec::ExecToLog '"$INSTDIR\isotone-devicetool.exe" restart-audio'
  Pop $0

  Delete "$SMPROGRAMS\${APPNAME}.lnk"
  DeleteRegKey HKLM "${ARPKEY}"
  DeleteRegKey HKLM "Software\${APPNAME}"
  DeleteRegValue HKCU "${RUNKEY}" "${APPNAME}"

  ; The tree, and only then the directory: RMDir /r on $INSTDIR while
  ; Uninstall.exe is running in it leaves the exe behind, which is why it is
  ; deleted on the next boot instead.
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR"
  ${If} ${FileExists} "$INSTDIR\*.*"
    Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
    RMDir /REBOOTOK "$INSTDIR"
  ${EndIf}
SectionEnd
