; SPDX-License-Identifier: GPL-2.0-or-later
; Copyright (C) 2026 The Isotone authors
;
; IsoAPO.dll is loaded by audiodg on every output it is installed on, and a
; loaded image cannot be opened for writing: an upgrade over a live engine
; stopped at "Error opening file for writing" on it (2026-09-21). It can be
; renamed, though, and the new file then written at its path; the renamed one
; can only be deleted once audiodg lets go of it (all four measured on a DLL held
; by LoadLibrary). tests/move_aside_test.nsi runs this against such a DLL.
;
; MoveAside <file> <var>: renames <file>, when it exists, to a new name in its
; directory and leaves that name in <var>, or "" when there was nothing to move.
; Aborts the install when the rename fails, rather than going on to fail at the
; copy.

!macro MoveAside file var
  StrCpy ${var} ""
  ${If} ${FileExists} "${file}"
    GetTempFileName ${var} "$INSTDIR"
    Delete ${var}
    ClearErrors
    Rename "${file}" ${var}
    ${If} ${Errors}
      DetailPrint "Could not move ${file} aside"
      MessageBox MB_ICONSTOP "Setup could not replace ${file}." /SD IDOK
      Abort "Setup could not replace ${file}."
    ${EndIf}
    DetailPrint "Moved the running engine aside: ${var}"
  ${EndIf}
!macroend
