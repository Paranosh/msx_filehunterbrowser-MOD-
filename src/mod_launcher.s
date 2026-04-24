;;; ==========================================================
;;; mod_launcher.s
;;; Launch SofaRun (SR.COM /S) from the current directory.
;;;
;;; MSX-DOS 2 has no BDOS _EXEC function, so we implement our
;;; own loader: copy a small stub to high RAM (0xE000, above any
;;; realistic SR.COM load range), then from the relocated stub:
;;;   1. Open SR.COM via BDOS _OPEN (0x43)
;;;   2. Read up to 32256 bytes into 0x0100 (TPA)
;;;   3. Close the file
;;;   4. Write "/S" command tail at 0x0080 in CLI format
;;;   5. Zero the default FCBs at 0x005C / 0x006C
;;;   6. JP 0x0100 to start SR.COM
;;;
;;; This routine NEVER returns on success — SR.COM takes over.
;;; On error, it terminates with BDOS _TERM0.
;;;
;;; Label references within the relocated stub use the form:
;;;    EXEC_ADDR + (label - stub_start)
;;; which resolves to a constant at assembly time (the runtime
;;; address once the stub has been LDIR'd to EXEC_ADDR).
;;; ==========================================================

    .module mod_launcher
    .globl _launchSofaRun
    .area _CODE

EXEC_ADDR = 0xE000

;;; void launchSofaRun(void);
_launchSofaRun::
    ;; Copy the stub from _CODE to high RAM
    ld      hl,#stub_start
    ld      de,#EXEC_ADDR
    ld      bc,#stub_end - stub_start
    ldir
    ;; Jump into the relocated stub
    jp      EXEC_ADDR

;;; ---- Relocated stub (runs at EXEC_ADDR) ----
stub_start:
    ;; --- Open SR.COM ---
    ld      de,#EXEC_ADDR + (stub_fname - stub_start)
    ld      a,#1                      ; open for read
    ld      c,#0x43                   ; _OPEN
    call    0x0005
    or      a
    jp      nz,EXEC_ADDR + (stub_err - stub_start)
    ;; B = file handle

    ;; --- Read up to 0x7E00 bytes into 0x0100 ---
    ld      de,#0x0100
    ld      hl,#0x7E00                ; 32256 bytes max (0x0100..0x7EFF)
    ld      c,#0x48                   ; _READ
    call    0x0005
    ;; Ignore partial-read / EOF: SR.COM will have loaded whatever fit

    ;; --- Close file handle ---
    ld      c,#0x45                   ; _CLOSE
    call    0x0005

    ;; --- Write command tail at 0x0080 ---
    ;; CLI buffer format: length_byte, text..., 0x0D (CR terminator)
    ld      hl,#EXEC_ADDR + (stub_cmdtail - stub_start)
    ld      de,#0x0080
    ld      bc,#stub_cmdtail_end - stub_cmdtail
    ldir

    ;; --- Clear default FCBs at 0x005C and 0x006C ---
    ;; MSX-DOS normally fills these with the parsed filename args;
    ;; since we have no file arg, zero them so SR.COM sees clean state.
    xor     a
    ld      hl,#0x005C
    ld      b,#0x20                   ; 2 * 16 bytes = both FCBs
.clr_fcb:
    ld      (hl),a
    inc     hl
    djnz    .clr_fcb

    ;; --- Jump to loaded program ---
    jp      0x0100

stub_err:
    ;; Abort back to MSX-DOS
    ld      c,#0x62                   ; _TERM
    ld      b,#1                      ; error code
    call    0x0005
    ;; If _TERM ever returns (shouldn't), fall through to _TERM0
    ld      c,#0x00
    call    0x0005

stub_fname:
    .ascii  "SR.COM"
    .db     0

stub_cmdtail:
    .db     3                         ; length of " /S" (not counting CR)
    .ascii  " /S"
    .db     0x0D                      ; CR terminator
stub_cmdtail_end:

stub_end:
