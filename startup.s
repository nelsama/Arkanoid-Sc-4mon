; ============================================
; startup.s - Código de inicio para programas C
; ============================================
; Inicializa el runtime CC65 para programas cargados en RAM
; Se ejecuta desde $0800
; ============================================

.export _init
.export __STARTUP__ : absolute = 1

.import _main
.import __BSS_RUN__, __BSS_SIZE__
.importzp c_sp

; Variables temporales en zero page
.segment "ZEROPAGE"
ptr1:       .res 2
ptr2:       .res 2
count:      .res 2

.segment "STARTUP"

_init:
    ; Deshabilitar interrupciones durante init
    sei
    cld

    ; Inicializar stack pointer del 6502
    ldx #$FF
    txs

    ; Inicializar stack pointer de CC65 (software stack)
    ; Tope del soft-stack en $3EFF (256 bytes: $3F00-$3FFF reservados)
    lda #<$3EFF
    sta c_sp
    lda #>$3EFF
    sta c_sp+1

    ; Inicializar BSS a ceros
    jsr zerobss

    ; Llamar a main
    jsr _main

    ; Si main retorna, volver al monitor.
    ; Se activa el AUTOBOOT escribiendo 0 en $C004 bit 0 (registro de hardware
    ; que persiste al reset) y se reentra por el VECTOR RESET con jmp ($FFFC).
    ; Asi el monitor relee BOOT.INI y ejecuta el launcher, en vez de quedar en
    ; el prompt. (jmp $8000 reentra igual, pero NO pasa por RESET ni procesa
    ; el flag de autoboot.)
    sei
    lda #$00
    sta $C004       ; $C004 bit0 = 0 -> permitir autoboot (leer BOOT.INI)
    jmp ($FFFC)     ; reset -> monitor + BOOT.INI

; ============================================
; zerobss - Inicializa BSS a ceros
; ============================================
zerobss:
    ; Si BSS_SIZE es 0, no hay nada que hacer
    lda #<__BSS_SIZE__
    ora #>__BSS_SIZE__
    beq @done

    ; Inicializar puntero al inicio de BSS
    lda #<__BSS_RUN__
    sta ptr1
    lda #>__BSS_RUN__
    sta ptr1+1

    ; Contador de bytes
    lda #<__BSS_SIZE__
    sta count
    lda #>__BSS_SIZE__
    sta count+1

    ; Llenar con ceros
    ldy #0
    lda #0
@loop:
    sta (ptr1),y

    ; Incrementar puntero
    inc ptr1
    bne @skip
    inc ptr1+1
@skip:

    ; Decrementar contador
    lda count
    bne @dec_low
    dec count+1
@dec_low:
    dec count

    ; Verificar si terminamos
    lda count
    ora count+1
    bne @loop

@done:
    rts
