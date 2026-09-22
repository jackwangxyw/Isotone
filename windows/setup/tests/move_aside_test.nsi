; SPDX-License-Identifier: GPL-2.0-or-later
; Copyright (C) 2026 The Isotone authors
;
; The installer's MoveAside (move_aside.nsh), unelevated: moves $INSTDIR\IsoAPO.dll
; aside and writes a new one at its path, as the installer's File /r does.
; Driven by test_move_aside.py, which holds the DLL loaded while this runs.
; Writes what happened to $INSTDIR\result.txt.

Unicode true
RequestExecutionLevel user
SilentInstall silent
OutFile "${OUTFILE}"
InstallDir "$TEMP\isotone-move-aside"

!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "..\move_aside.nsh"

Var Old

Section
  SetOutPath "$INSTDIR"
  !insertmacro MoveAside "$INSTDIR\IsoAPO.dll" $Old
  File /oname=IsoAPO.dll "move_aside_payload.txt"
  FileOpen $0 "$INSTDIR\result.txt" w
  FileWrite $0 "moved=$Old"
  FileClose $0
SectionEnd
