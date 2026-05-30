import os

rom_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'roms', 'Batman - Return of the Joker (USA).nes')
with open(rom_path, 'rb') as f:
    data = bytearray(f.read())

header = 16

opcodes = {
    0xA9:'LDA #', 0xA5:'LDA zp', 0xB5:'LDA zp,X', 0xAD:'LDA abs', 0xBD:'LDA abs,X', 0xB9:'LDA abs,Y',
    0x85:'STA zp', 0x95:'STA zp,X', 0x8D:'STA abs', 0x9D:'STA abs,X', 0x99:'STA abs,Y',
    0xA2:'LDX #', 0xA6:'LDX zp', 0xAE:'LDX abs', 0x86:'STX zp', 0x8E:'STX abs',
    0xA0:'LDY #', 0xA4:'LDY zp', 0xAC:'LDY abs', 0x84:'STY zp', 0x8C:'STY abs',
    0xE8:'INX', 0xCA:'DEX', 0xC8:'INY', 0x88:'DEY',
    0x4C:'JMP abs', 0x6C:'JMP ind', 0x20:'JSR abs', 0x60:'RTS', 0x40:'RTI',
    0xD0:'BNE', 0xF0:'BEQ', 0x10:'BPL', 0x30:'BMI', 0x90:'BCC', 0xB0:'BCS', 0x50:'BVC', 0x70:'BVS',
    0xE9:'SBC #', 0xE5:'SBC zp', 0x69:'ADC #', 0x65:'ADC zp', 0x6D:'ADC abs',
    0x29:'AND #', 0x25:'AND zp', 0x09:'ORA #', 0x05:'ORA zp', 0x49:'EOR #', 0x45:'EOR zp',
    0x38:'SEC', 0x18:'CLC', 0xF8:'SED', 0xD8:'CLD', 0x78:'SEI', 0x58:'CLI',
    0xAA:'TAX', 0x8A:'TXA', 0xA8:'TAY', 0x98:'TYA', 0x9A:'TXS', 0xBA:'TSX',
    0x48:'PHA', 0x68:'PLA', 0x08:'PHP', 0x28:'PLP',
    0xE6:'INC zp', 0xEE:'INC abs', 0xC6:'DEC zp', 0xCE:'DEC abs',
    0xC9:'CMP #', 0xC5:'CMP zp', 0xCC:'CPY abs', 0xEC:'CPX abs', 0xE0:'CPX #', 0xC0:'CPY #',
    0x24:'BIT zp', 0x2C:'BIT abs', 0xEA:'NOP',
    0x4A:'LSR A', 0x0A:'ASL A', 0x2A:'ROL A', 0x6A:'ROR A',
    0x46:'LSR zp', 0x56:'LSR zp,X', 0x4E:'LSR abs',
    0x26:'ROL zp', 0x66:'ROR zp', 0xBC:'LDY abs,X',
    0xD6:'DEC zp,X', 0xF6:'INC zp,X', 0xFE:'INC abs,X', 0xDE:'DEC abs,X',
    0x91:'STA (zp),Y', 0x81:'STA (zp,X)', 0xA1:'LDA (zp,X)', 0xB1:'LDA (zp),Y',
    0x00:'BRK', 0xCD:'CMP abs', 0xDD:'CMP abs,X',
    0x94:'STY zp,X', 0x96:'STX zp,Y',  # add missing indexed stores
    0x7E:'ROR abs,X', 0x1E:'ASL abs,X', 0x3E:'ROL abs,X', 0x5E:'LSR abs,X',
    0xD1:'CMP (zp),Y', 0xC1:'CMP (zp,X)',
    0xF5:'SBC zp,X', 0x75:'ADC zp,X', 0x35:'AND zp,X', 0x15:'ORA zp,X', 0x55:'EOR zp,X',
    0xB4:'LDY zp,X', 0xA1:'LDA (zp,X)', 0xB1:'LDA (zp),Y',
    0x11:'ORA (zp),Y', 0x31:'AND (zp),Y', 0x51:'EOR (zp),Y', 0x71:'ADC (zp),Y',
    0xF1:'SBC (zp),Y', 0xD1:'CMP (zp),Y',
}
sizes = {
    'LDA #':2,'LDA zp':2,'LDA zp,X':2,'LDA abs':3,'LDA abs,X':3,'LDA abs,Y':3,
    'STA zp':2,'STA zp,X':2,'STA abs':3,'STA abs,X':3,'STA abs,Y':3,
    'LDX #':2,'LDX zp':2,'LDX abs':3,'STX zp':2,'STX abs':3,'STX zp,Y':2,
    'LDY #':2,'LDY zp':2,'LDY abs':3,'STY zp':2,'STY abs':3,'STY zp,X':2,
    'LDY zp,X':2,
    'INX':1,'DEX':1,'INY':1,'DEY':1,
    'JMP abs':3,'JMP ind':3,'JSR abs':3,'RTS':1,'RTI':1,
    'BNE':2,'BEQ':2,'BPL':2,'BMI':2,'BCC':2,'BCS':2,'BVC':2,'BVS':2,
    'SBC #':2,'SBC zp':2,'ADC #':2,'ADC zp':2,'ADC abs':3,
    'AND #':2,'AND zp':2,'ORA #':2,'ORA zp':2,'EOR #':2,'EOR zp':2,
    'SEC':1,'CLC':1,'SED':1,'CLD':1,'SEI':1,'CLI':1,
    'TAX':1,'TXA':1,'TAY':1,'TYA':1,'TXS':1,'TSX':1,
    'PHA':1,'PLA':1,'PHP':1,'PLP':1,
    'INC zp':2,'INC abs':3,'DEC zp':2,'DEC abs':3,'INC abs,X':3,'DEC abs,X':3,
    'CMP #':2,'CMP zp':2,'CMP abs':3,'CMP abs,X':3,'CMP (zp),Y':2,'CMP (zp,X)':2,
    'CPY abs':3,'CPX abs':3,'CPX #':2,'CPY #':2,
    'BIT zp':2,'BIT abs':3,'NOP':1,
    'LSR A':1,'ASL A':1,'ROL A':1,'ROR A':1,
    'LSR zp':2,'LSR zp,X':2,'LSR abs':3,'ROL zp':2,'ROR zp':2,
    'LDY abs,X':3,'INC zp,X':2,'DEC zp,X':2,
    'ROR abs,X':3,'ASL abs,X':3,'ROL abs,X':3,'LSR abs,X':3,
    'STA (zp),Y':2,'STA (zp,X)':2,'LDA (zp,X)':2,'LDA (zp),Y':2,
    'BRK':2,'CMP abs':3,
    'SBC zp,X':2,'ADC zp,X':2,'AND zp,X':2,'ORA zp,X':2,'EOR zp,X':2,
    'ORA (zp),Y':2,'AND (zp),Y':2,'EOR (zp),Y':2,'ADC (zp),Y':2,
    'SBC (zp),Y':2,
}

def disasm(bank_idx, start_vaddr, slot_base, count, label=''):
    bank_off = header + bank_idx * 0x2000
    if label:
        print(f'\n=== {label} ===')
    pos = start_vaddr - slot_base
    if pos < 0 or pos >= 0x2000:
        print(f'  [out of range]')
        return
    addr = start_vaddr
    printed = 0
    while printed < count and 0 <= pos < 0x2000:
        op = data[bank_off + pos]
        mn = opcodes.get(op, f'db ${op:02X}')
        sz = sizes.get(mn, 1)
        b = [data[bank_off + pos + j] for j in range(min(sz, 0x2000 - pos))]
        bs = ' '.join(f'{x:02X}' for x in b)
        if sz == 1:
            print(f'  ${addr:04X}: {bs:<8}  {mn}')
        elif sz == 2:
            op2 = b[1] if len(b) > 1 else 0
            if mn in ('BNE','BEQ','BPL','BMI','BCC','BCS','BVC','BVS'):
                tgt = addr + 2 + (op2 if op2 < 128 else op2-256)
                print(f'  ${addr:04X}: {bs:<8}  {mn} ${tgt:04X}')
            else:
                print(f'  ${addr:04X}: {bs:<8}  {mn} ${op2:02X}')
        elif sz == 3:
            lo = b[1] if len(b) > 1 else 0
            hi = b[2] if len(b) > 2 else 0
            tgt = lo | (hi << 8)
            print(f'  ${addr:04X}: {bs:<8}  {mn} ${tgt:04X}')
        pos += sz
        addr += sz
        printed += 1
        if mn in ('RTS','RTI','JMP abs','JMP ind','BRK'):
            print()

# Bank 14 is in slot3 ($E000) during the game
# Show what bank14 has around the bank-swap handoff area
disasm(14, 0xE300, 0xE000, 80, 'Bank 14 around $E33F (bank-swap handoff area)')

# Bank 14 main init entry - what does it call from $E33F onwards?
# If JSR $00A2 returns, execution continues at $E342
b14off = header + 14 * 0x2000
print('\n=== Bank 14 raw bytes $E33F-$E380 ===')
for i in range(0x3F, 0x80):
    addr = 0xE000 + i
    b = data[b14off + i]
    print(f'  ${addr:04X}: ${b:02X}', end='')
    if i % 8 == 7:
        print()
print()

# What does bank14 do further after init?
# The real init likely branches off to somewhere substantial
# Let's look at $E000-$E100 in bank14
disasm(14, 0xE000, 0xE000, 60, 'Bank 14 at $E000 (start of slot3)')

# Bank 15 NMI handler at $E1D0
disasm(15, 0xE1D0, 0xE000, 60, 'Bank 15 NMI handler ($E1D0)')

# Bank 15 IRQ handler at $E2A9
disasm(15, 0xE2A9, 0xE000, 40, 'Bank 15 IRQ handler ($E2A9)')

# What does bank14's $D394 call? (In bank13 at $C000 = slot2)
# $D394 is in slot2 = bank13
disasm(13, 0xD394, 0xC000, 60, 'Bank 13 at $D394 (called by bank15 after loop)')

# More importantly: what's the full bank15 init path after the bank setup loop?
# Bank15 never has control after $E33C, but let's check bank14's $E347 onwards
disasm(14, 0xE340, 0xE000, 80, 'Bank 14 from $E340 (post-JSR $00A2 area)')
