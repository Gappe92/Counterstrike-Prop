import wave

wav = wave.open(r"C:\Users\Sebastian\Downloads\0007.wav", "rb")  # Windows^1
frames = wav.readframes(wav.getnframes())
data = list(frames)

print("const uint8_t clickSound[] = {")
for i, b in enumerate(data):
    print(f"{b},", end="")
    if (i+1) % 16 == 0:
        print()
print("};")
