import os

rom_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'roms', 'Batman - Return of the Joker (USA).nes')
with open(rom_path, 'rb') as f:
    data = bytearray(f.read())

header = 16
b11off = header + 11 * 0x2000
b12off = header + 12 * 0x2000

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
    0xBC:'LDY abs,X', 0xD6:'DEC zp,X', 0xF6:'INC zp,X',
    0x91:'STA (zp),Y', 0x81:'STA (zp,X)', 0xA1:'LDA (zp,X)', 0xB1:'LDA (zp),Y',
    0x00:'BRK', 0xCD:'CMP abs', 0xDD:'CMP abs,X',
    0x46:'LSR zp', 0x56:'LSR zp,X', 0x4E:'LSR abs',
    0x26:'ROL zp', 0x66:'ROR zp',
}
sizes = {
    'LDA #':2,'LDA zp':2,'LDA zp,X':2,'LDA abs':3,'LDA abs,X':3,'LDA abs,Y':3,
    'STA zp':2,'STA zp,X':2,'STA abs':3,'STA abs,X':3,'STA abs,Y':3,
    'LDX #':2,'LDX zp':2,'LDX abs':3,'STX zp':2,'STX abs':3,
    'LDY #':2,'LDY zp':2,'LDY abs':3,'STY zp':2,'STY abs':3,
    'INX':1,'DEX':1,'INY':1,'DEY':1,
    'JMP abs':3,'JMP ind':3,'JSR abs':3,'RTS':1,'RTI':1,
    'BNE':2,'BEQ':2,'BPL':2,'BMI':2,'BCC':2,'BCS':2,'BVC':2,'BVS':2,
    'SBC #':2,'SBC zp':2,'ADC #':2,'ADC zp':2,'ADC abs':3,
    'AND #':2,'AND zp':2,'ORA #':2,'ORA zp':2,'EOR #':2,'EOR zp':2,
    'SEC':1,'CLC':1,'SED':1,'CLD':1,'SEI':1,'CLI':1,
    'TAX':1,'TXA':1,'TAY':1,'TYA':1,'TXS':1,'TSX':1,
    'PHA':1,'PLA':1,'PHP':1,'PLP':1,
    'INC zp':2,'INC abs':3,'DEC zp':2,'DEC abs':3,
    'CMP #':2,'CMP zp':2,'CMP abs':3,'CMP abs,X':3,
    'CPY abs':3,'CPX abs':3,'CPX #':2,'CPY #':2,
    'BIT zp':2,'BIT abs':3,'NOP':1,
    'LSR A':1,'ASL A':1,'ROL A':1,'ROR A':1,
    'LSR zp':2,'LSR zp,X':2,'LSR abs':3,'ROL zp':2,'ROR zp':2,
    'LDY abs,X':3,'INC zp,X':2,'DEC zp,X':2,
    'STA (zp),Y':2,'STA (zp,X)':2,'LDA (zp,X)':2,'LDA (zp),Y':2,
    'BRK':2,'CMP abs':3,'CMP abs,X':3,
}

def disasm(base_off, start_addr, base_rom_addr, count, label=''):
    if label:
        print(f'\n=== {label} ===')
    pos = start_addr - base_rom_addr
    addr = start_addr
    for _ in range(count):
        if pos >= 0x2000 or pos < 0:
            break
        op = data[base_off + pos]
        mn = opcodes.get(op, 'DB')
        sz = sizes.get(mn, 1)
        b = [data[base_off+pos+j] for j in range(min(sz, 3))]
        bs = ' '.join(f'{x:02X}' for x in b)
        if sz == 1:
            print(f'  ${addr:04X}: {bs:<8}  {mn}')
        elif sz == 2:
            op2 = b[1]
            if mn in ('BNE','BEQ','BPL','BMI','BCC','BCS','BVC','BVS'):
                tgt = addr + 2 + (op2 if op2 < 128 else op2-256)
                print(f'  ${addr:04X}: {bs:<8}  {mn} ${tgt:04X}')
            else:
                print(f'  ${addr:04X}: {bs:<8}  {mn} ${op2:02X}')
        else:
            lo, hi = b[1], b[2]
            tgt = lo | (hi<<8)
            print(f'  ${addr:04X}: {bs:<8}  {mn} ${tgt:04X}')
        pos += sz
        addr += sz
        if mn in ('RTS','RTI','JMP abs','JMP ind'):
            print()

# Bank 11: path after $9DA4 that leads to $9DC7
disasm(b11off, 0x9DC7, 0x8000, 40, 'Bank 11 $9DC7 (scroll update)')

# Bank 11: what is at $9D3B
disasm(b11off, 0x9CF0, 0x8000, 80, 'Bank 11 $9CF0 (context before $9D3B)')

# Bank 12: full $A79B function
disasm(b12off, 0xA79B, 0xA000, 15, 'Bank 12 $A79B (writes AA to 7CC0)')

# Bank 12: what is at $A043 (STA 7CC0,Y)?
disasm(b12off, 0xA020, 0xA000, 40, 'Bank 12 $A020 (task search/create)')
