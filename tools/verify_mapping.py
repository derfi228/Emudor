import os
rom_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'roms', 'Batman - Return of the Joker (USA).nes')
with open(rom_path, 'rb') as f:
    data = bytearray(f.read())
header = 16

# Check whether the dispatcher (CMP #$07) is in bank11 or bank12 at offset $1D4C
for bname, bidx in [('bank11', 11), ('bank12', 12)]:
    off = header + bidx * 0x2000 + 0x1D4C
    b = data[off]
    print('%s offset $1D4C: %02X (%s)' % (bname, b, 'CMP opcode!' if b == 0xC9 else 'not CMP'))

# Check bank14 at offset $0EDE ($CEDE - $C000)
b14_off = header + 14 * 0x2000
cede_in_b14 = data[b14_off + 0x0EDE]
print('\nbank14[$0EDE] ($CEDE if bank14 at C000): %02X (%s)' % (cede_in_b14, 'BRK!' if cede_in_b14 == 0 else 'not BRK'))

# Check bank13 at offset $0EDE
b13_off = header + 13 * 0x2000
cede_in_b13 = data[b13_off + 0x0EDE]
print('bank13[$0EDE] ($CEDE if bank13 at C000): %02X (%s)' % (cede_in_b13, 'BRK!' if cede_in_b13 == 0 else 'not BRK'))

# Check $E33F if $E000-$FFFF is FIXED (bank15)
# With fixed $E000-$FFFF = bank15, iter4 STX $A000 writes cmd $0B data $1E -> $C000-$DFFF = bank14
# The loop continues in bank15 -- $E33F is still bank15
b15_off = header + 15 * 0x2000
print('\nbank15[$33F] (= STX $E0,Y opcode $96?): %02X' % data[b15_off + 0x33F])
print('bank15[$341] (= INX opcode $E8?): %02X' % data[b15_off + 0x341])
print('bank15[$342] (= INY opcode $C8?): %02X' % data[b15_off + 0x342])
print('bank15[$343-$344] (= CPX #$1F = $E0 $1F?): %02X %02X' %
      (data[b15_off + 0x343], data[b15_off + 0x344]))
print('bank15[$345-$346] (= BNE $E339 = $D0 $F2?): %02X %02X' %
      (data[b15_off + 0x345], data[b15_off + 0x346]))
print('bank15[$347-$348] (= LDA #$00 = $A9 $00, 2nd init?): %02X %02X' %
      (data[b15_off + 0x347], data[b15_off + 0x348]))

# Check what bank14 has at $C000-$DFFF range for $DDF4 (offset $1DF4)
b14_ddf4 = data[b14_off + 0x1DF4]
print('\nbank14 at $DDF4 (offset $1DF4): %02X (D3=0 dispatch target)' % b14_ddf4)

# Check bank13 at $DDF4
b13_ddf4 = data[b13_off + 0x1DF4]
print('bank13 at $DDF4 (offset $1DF4): %02X' % b13_ddf4)

# Confirm: With CORRECT mapping, what's at $8000-$9FFF?
# Bank12 (prgBank_[1]) at $8000-$9FFF
# $9D4C - $8000 = $1D4C offset in bank12
b12_off = header + 12 * 0x2000
print('\nbank12 first 6 bytes (at $8000 with correct mapping):',
      ' '.join('%02X' % data[b12_off + i] for i in range(6)))
print('bank12[$1D4C] (= $9D4C with correct mapping): %02X' % data[b12_off + 0x1D4C])

# With CORRECT mapping, what's at $A000-$BFFF?
# Bank13 (prgBank_[2]) at $A000-$BFFF
print('\nbank13 first 6 bytes (at $A000 with correct mapping):',
      ' '.join('%02X' % data[b13_off + i] for i in range(6)))

# With CORRECT mapping: bank11 at $6000-$7FFF
b11_off = header + 11 * 0x2000
print('\nbank11 first 6 bytes (at $6000 with correct mapping):',
      ' '.join('%02X' % data[b11_off + i] for i in range(6)))

# Sanity check: does bank15's second init phase JSR $C117 (= $C117 - $A000 = $0117 in bank13)?
# With correct mapping: $A000-$BFFF = bank13
# $C117 - ??? hmm, $C117 is in $C000-$DFFF range = bank14 with correct mapping
print('\nbank14 at $C117 (offset $0117):',
      ' '.join('%02X' % data[b14_off + 0x0117 + i] for i in range(6)))
