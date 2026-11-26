/*
 * LED driver table commands of the AWTRIX MCU extension, build 14 (RV32IMC).
 *
 * The stock firmware V1.0.17 keeps the panel drivers' registers in a RAM table (0x15794 red,
 * +0x44 green, +0x88 blue; registers 0x02..0x20) and sends it to the drivers at start and whenever
 * the panel comes back on. Its own 0x12 saves one "override" to flash instead of changing the
 * table, and 0x13 answers with that override once one exists. These handlers reach the table
 * directly through the firmware's own routines and never write flash, except 0x0b, which clears
 * that override.
 *
 * Calling convention of the stock dispatcher (0x10003bb6): s0 = frame (FF 55 cmd len payload),
 * s2 = payload length, s1 = frame length. A handler ends with a jump to 0x10003ba0, which consumes
 * the frame; s1 and tp must survive. 0x10003a14(cmd, buffer, length) sends an answer frame.
 *
 * build.py links this right after build 13 (0x100087c0) and checks each handler's address against
 * the one McuPatch.cpp writes into the dispatch table:
 *   0x09 write_register: three big-endian words reg<<8|value (red, green, blue), like 0x12;
 *                       sets the table, sends the register to the drivers, echoes the words.
 *   0x0a read_register: the register once per colour, like 0x13; answers with the table's
 *                       three words, whatever override exists.
 *   0x0b clear_override: no payload; empties the saved override (settings offset 0x300) so
 *                       0x13 reads the table again, and answers with no payload.
 * A malformed request is answered with the same command and no payload.
 */

        .equ CONSUME,       0x10003ba0
        .equ SEND,          0x10003a14
        .equ TABLE_SET,     0x10004adc   /* (red, green, blue words) */
        .equ TABLE_GET,     0x10004b06   /* (&red, &green, &blue) -> 1, or 0 outside the range */
        .equ DRIVER_SEND,   0x10003af4   /* (red, green, blue words): the live part of 0x12 */
        .equ SETTINGS_PUT,  0x10005458   /* (buffer, offset, length) into the settings cache */
        .equ SETTINGS_SAVE, 0x100053da   /* writes the settings cache when it changed */

        .option norelax
        .text
        .globl  handlers, write_register, read_register, clear_override, handlers_end
handlers:

/* ---- 0x09: write the table and the drivers ---------------------------------------------- */
        .balign 4
write_register:
        li      t0, 6
        bne     s2, t0, write_reject
        lbu     t1, 4(s0)              /* register, three times */
        lbu     t2, 6(s0)
        bne     t1, t2, write_reject
        lbu     t2, 8(s0)
        bne     t1, t2, write_reject
        addi    t2, t1, -2
        li      t0, 0x1e
        bltu    t0, t2, write_reject
        addi    sp, sp, -16
        sw      ra, 12(sp)
        slli    t1, t1, 8
        lbu     a0, 5(s0)
        or      a0, a0, t1
        lbu     a1, 7(s0)
        or      a1, a1, t1
        lbu     a2, 9(s0)
        or      a2, a2, t1
        sw      a0, 0(sp)
        sw      a1, 4(sp)
        sw      a2, 8(sp)
        li      t0, TABLE_SET
        jalr    t0
        lw      a0, 0(sp)
        lw      a1, 4(sp)
        lw      a2, 8(sp)
        li      t0, DRIVER_SEND
        jalr    t0
        li      a0, 0x09
        addi    a1, s0, 4
        li      a2, 6
        li      t0, SEND
        jalr    t0
        lw      ra, 12(sp)
        addi    sp, sp, 16
        li      t0, CONSUME
        jr      t0
write_reject:
        li      a0, 0x09
        j       reject

/* ---- 0x0a: read the table ---------------------------------------------------------------- */
        .balign 4
read_register:
        li      t0, 3
        bne     s2, t0, read_reject
        lbu     t1, 4(s0)
        lbu     t2, 5(s0)
        bne     t1, t2, read_reject
        lbu     t2, 6(s0)
        bne     t1, t2, read_reject
        addi    sp, sp, -16
        sw      ra, 12(sp)
        slli    t1, t1, 8
        sh      t1, 0(sp)
        sh      t1, 2(sp)
        sh      t1, 4(sp)
        mv      a0, sp
        addi    a1, sp, 2
        addi    a2, sp, 4
        li      t0, TABLE_GET
        jalr    t0
        beqz    a0, read_failed
        lhu     t0, 0(sp)              /* three words, big-endian, at sp+6 */
        srli    t1, t0, 8
        sb      t1, 6(sp)
        sb      t0, 7(sp)
        lhu     t0, 2(sp)
        srli    t1, t0, 8
        sb      t1, 8(sp)
        sb      t0, 9(sp)
        lhu     t0, 4(sp)
        srli    t1, t0, 8
        sb      t1, 10(sp)
        sb      t0, 11(sp)
        li      a0, 0x0a
        addi    a1, sp, 6
        li      a2, 6
        li      t0, SEND
        jalr    t0
        lw      ra, 12(sp)
        addi    sp, sp, 16
        li      t0, CONSUME
        jr      t0
read_failed:
        lw      ra, 12(sp)
        addi    sp, sp, 16
read_reject:
        li      a0, 0x0a
        j       reject

/* ---- 0x0b: clear the saved override ------------------------------------------------------ */
        .balign 4
clear_override:
        bnez    s2, clear_reject
        addi    sp, sp, -32
        sw      ra, 28(sp)
        sw      zero, 0(sp)            /* no 0xe5 marker: no override */
        sw      zero, 4(sp)
        sw      zero, 8(sp)
        sw      zero, 12(sp)
        mv      a0, sp
        li      a1, 0x300
        li      a2, 16
        li      t0, SETTINGS_PUT
        jalr    t0
        li      t0, SETTINGS_SAVE
        jalr    t0
        lw      ra, 28(sp)
        addi    sp, sp, 32
clear_reject:
        li      a0, 0x0b

/* An answer with no payload: refused, or done for 0x0b. a0 = command. */
reject:
        addi    sp, sp, -16
        sw      ra, 12(sp)
        mv      a1, s0
        li      a2, 0
        li      t0, SEND
        jalr    t0
        lw      ra, 12(sp)
        addi    sp, sp, 16
        li      t0, CONSUME
        jr      t0
handlers_end:
