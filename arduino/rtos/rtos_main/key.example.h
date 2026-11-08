// key.example.h - template for key.h (the real key is NOT in git).
// Copy this file to key.h in the same folder and replace the 16 bytes with your own random ones,
// e.g. from:  python3 -c "import secrets; print(', '.join('0x%02x' % b for b in secrets.token_bytes(16)))"
// Every box must be flashed with the SAME key.
#pragma once
#define MESH_KEY { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
