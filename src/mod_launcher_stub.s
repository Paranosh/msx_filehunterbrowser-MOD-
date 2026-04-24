;;; ==========================================================
;;; mod_launcher_stub.s
;;;
;;; Tiny trampoline that launches SR.COM /S.
;;;
;;; Page-zero layout (set up by mod_launcher.c before calling us):
;;;   0x0080  cmdtail  : 0x03, " /S", 0x0D            (5 bytes, written here)
;;;   0x0085  stub code: this stub, copied here by LDIR (~47 bytes)
;;;   0x00C0  FNAME_ADDR: ASCIIZ full path to SR.COM   (written by C)
;;;
;;; Page zero (0x0000-0x00FF) is never overwritten when a .COM loads at
;;; 0x0100, so the stub runs safely all the way through the JP 0x0100.
;;;
;;; All intra-stub branches use JR (PC-relative) so the code is
;;; position-independent and survives the LDIR relocation to 0x0085.
;;; All absolute references are fixed constants (0x0005, 0x00C0, 0x0100…).
;;; ==========================================================

	.module mod_launcher_stub
	.globl _launchSofaRunASM
	.area _CODE

STUB_ADDR  = 0x0085   ; page-zero destination for the relocated stub
FNAME_ADDR = 0x00C0   ; where mod_launcher.c wrote the SR.COM path

;;; void launchSofaRunASM(void)
;;; Called from C after FNAME_ADDR has been filled with a valid ASCIIZ path.
;;; Never returns on success.
_launchSofaRunASM::
	;; Write cmdtail at 0x0080: length=3, " /S", CR
	ld   hl, #_cmdtail
	ld   de, #0x0080
	ld   bc, #5
	ldir

	;; Copy stub to 0x0085 (page-zero DTA area — safe from .COM load)
	ld   hl, #_stub_start
	ld   de, #STUB_ADDR
	ld   bc, #_stub_end - _stub_start
	ldir

	;; Jump into the relocated stub
	jp   STUB_ADDR

_cmdtail:
	.db  3            ; length of " /S"
	.ascii " /S"
	.db  0x0D         ; CR

;;; ── Relocated stub ──────────────────────────────────────────
;;; Runs at STUB_ADDR (0x0085) after being LDIR'd there.
;;; Uses only JR for branches and hard-coded constants for addresses.
_stub_start:
	;; Open SR.COM (path at FNAME_ADDR = 0x00C0, always a fixed constant)
	ld   de, #FNAME_ADDR     ; 0x00C0
	ld   a, #1               ; open for reading (b0 = no-write)
	ld   c, #0x43            ; BDOS _OPEN
	call 0x0005
	or   a
	jr   nz, _stub_err       ; error → exit to DOS  (JR = PC-relative ✓)

	;; Read up to 32 KB into TPA (B = handle returned by _OPEN)
	ld   de, #0x0100         ; load address
	ld   hl, #0x7E00         ; max bytes (32256)
	ld   c, #0x48            ; BDOS _READ
	call 0x0005

	;; Close the file handle
	ld   c, #0x45            ; BDOS _CLOSE
	call 0x0005

	;; Clear default FCBs at 0x005C–0x007B (2 × 16 bytes = 32 bytes)
	xor  a
	ld   hl, #0x005C
	ld   b, #0x20
_clr_fcb:
	ld   (hl), a
	inc  hl
	djnz _clr_fcb            ; djnz = PC-relative ✓

	;; Hand control to SR.COM
	jp   0x0100

_stub_err:
	;; Return to MSX-DOS (TERM0 — exit code 0)
	ld   c, #0x00            ; BDOS _TERM0
	call 0x0005
_stub_end:
