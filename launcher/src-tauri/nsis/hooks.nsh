; Kiln Launcher installer hooks (tauri.conf.json bundle.windows.nsis.installerHooks).
;
; "Come back after installing Kiln" (Crucible, DESIGN 1.3): a download from a pack page's "Get Kiln" is named
; ...-setup.crucible-<CODE>.exe. The installer leaves its own file name beside the launcher; the launcher reads that note
; once at its next start (kiln_manage::crucible::links::RESUME_FILE), removes it and opens that pack's preview. Every
; install writes the note; an ordinary file name (or the updater's) carries no code and is simply removed.

!macro NSIS_HOOK_POSTINSTALL
  ClearErrors
  FileOpen $0 "$INSTDIR\crucible-resume.txt" w
  IfErrors +3
  FileWrite $0 "$EXEFILE"
  FileClose $0
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  Delete "$INSTDIR\crucible-resume.txt"
!macroend
