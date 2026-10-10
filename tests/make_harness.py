"""Write harness.c: the C part from harness.in.c plus generated assembly.

The harness stands in for BrgGame-Steam.exe. Its fake currency bar is written
with the same machine-code bytes the plugin looks for in the real game (the
draw before the Death Metal icon, the draw after it and the slot loop), and so
is its fake text-icon set-up (the Death Metal entry), so the
plugin finds and patches them exactly as it would there.

Usage: python tests/make_harness.py OUTPUT.c
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def asm(lines):
    return ''.join('    "%s\\n"\n' % l.replace('"', '\\"') for l in lines)


def b(hexes):
    return '.byte ' + ','.join('0x' + h for h in hexes.split())


bar = [
    '.globl fake_bar', 'fake_bar:',
    'pushq %rbx', 'pushq %rsi', 'pushq %r12', 'subq $0x60, %rsp',
    'movdqu %xmm7, 0x40(%rsp)', 'movdqu %xmm8, 0x50(%rsp)',
    'movq %rcx, %rbx',
    'movl g_r12(%rip), %r12d',
    'movss g_xmm7(%rip), %xmm7', 'movss g_xmm8(%rip), %xmm8',
    # the draw before the icon: call [rax+0x268]
    'movq (%rbx), %rax', 'movq %rbx, %rcx',
    b('FF 90 68 02 00 00'),
    # the draw after it, as in the game (the hook goes in at its first mov)
    b('48 8B 93 28 17 00 00  48 8B 03  4C 8B 8B 20 17 00 00  44 89 64 24 38  44 89 64 24 30'),
    b('F3 44 0F 11 44 24 28  F3 0F 11 7C 24 20  4D 8B 89 A4 07 00 00  4C 8B 82 B4 07 00 00'),
    b('48 8B 92 AC 07 00 00  48 8B CB  FF 90 70 02 00 00'),
    # the slot loop
    'xorl %esi, %esi',
    '1:', 'cmpl $6, %esi', 'je 9f',
    'call fake_cond', 'testl %eax, %eax', 'je 2f',
    b('83 FE 05 0F 84'), '.long 3f - (. + 4)',
    '2:', b('85 F6 0F 84'), '.long 3f - (. + 4)',
    b('48 8B 43 68'),
    'movl %esi, %ecx', 'call record_slot',
    '3:', 'incl %esi', 'jmp 1b',
    '9:',
    'movdqu 0x40(%rsp), %xmm7', 'movdqu 0x50(%rsp), %xmm8',
    'addq $0x60, %rsp', 'popq %r12', 'popq %rsi', 'popq %rbx', 'ret',
]

# the HUD's text-icon set-up, the Death Metal entry: hud in rcx, as in the game
# (rbx = the HUD; the layout object at [[[rbx+0x1548]+0x2d18]+0x148])
texticon = [
    '.globl fake_texticon', 'fake_texticon:',
    'pushq %rbx', 'movq %rcx, %rbx',
    b('48 8B 83 48 15 00 00  48 8B 88 18 2D 00 00  48 8B 81 48 01 00 00  48 8B 88 AC 07 00 00'),
    b('48 89 8B 68 0E 00 00'),
    b('48 8B 83 48 15 00 00  48 8B 88 18 2D 00 00  48 8B 81 48 01 00 00  48 8B 88 B4 07 00 00'),
    b('48 89 8B 70 0E 00 00'),
    'popq %rbx', 'ret',
]

body = (HERE / 'harness.in.c').read_text().replace('@ASM@', asm(['.text'] + bar + texticon))
Path(sys.argv[1]).write_text(body)
