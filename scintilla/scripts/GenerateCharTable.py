#!/usr/bin/env python3

from FileGenerator import Regenerate

def GenerateUTF8Table():
    def UTF8CharLength(ch):
        if ch < 0x80:
            return 1
        if ch < 0b11100000:
            return 2
        if ch < 0b11110000:
            return 3
        return 4

    def BytesFromLead(leadByte):
        # Single byte or invalid
        if leadByte < 0xC2:
            return 1
        if leadByte < 0b11100000:
            return 2
        if leadByte < 0b11110000:
            return 3
        if leadByte < 0xF5:
            return 4
        # Characters longer than 4 bytes not possible in current UTF-8
        return 1

    UTF8CharLengthTable = []
    UTF8BytesOfLead = []
    for i in range(0, 255, 16):
        line = ', '.join('%d' % UTF8CharLength(ch) for ch in range(i, i+16)) + ','
        UTF8CharLengthTable.append(line)

        line = ', '.join('%d' % BytesFromLead(ch) for ch in range(i, i+16)) + ','
        UTF8BytesOfLead.append(line)

    print('UTF8CharLength:', len(UTF8CharLengthTable))
    print('UTF8BytesOfLead:', len(UTF8BytesOfLead))
    Regenerate("../src/UniConversion.cxx", "//", UTF8CharLengthTable, UTF8BytesOfLead)

if __name__ == '__main__':
    GenerateUTF8Table()
