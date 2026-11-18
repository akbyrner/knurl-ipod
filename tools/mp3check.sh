#!/bin/sh
# Check the MP3 decoder and player against ffmpeg on every testdata/mp3/*.mp3
# (seeking: tools/seek_test, e.g. seek_test real1.mp3 30000 out.raw, then compare with ref2.raw):
#   decoder: our frames vs ffmpeg's (aligned), max difference in 16-bit steps
#   player:  gapless output vs ffmpeg's gapless output - same start, same length -
#            and the same again with a source that keeps running dry
set -e
cd "$(dirname "$0")/.."
B=firmware/build/mp3check
mkdir -p $B
python3 tools/build_pow43.py $B
gcc -O2 -std=c11 -Wall -Wextra -Ifirmware/src tools/mp3dec_test.c firmware/src/mp3/mp3dec.c $B/pow43.c -o $B/mp3dec_test
gcc -O2 -std=c11 -Wall -Wextra -Ifirmware/src tools/seek_test.c firmware/src/player.c firmware/src/mp3/mp3dec.c firmware/src/mp3/mp3info.c $B/pow43.c -o $B/seek_test
gcc -O2 -std=c11 -Wall -Wextra -Ifirmware/src tools/player_test.c firmware/src/player.c firmware/src/mp3/mp3dec.c firmware/src/mp3/mp3info.c $B/pow43.c -o $B/player_test
for f in testdata/mp3/*.mp3; do
    b=$B/$(basename "$f" .mp3)
    ch=$(ffprobe -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 "$f")
    echo "== $(basename "$f")"
    printf '  decoder  '; $B/mp3dec_test "$f" "$b.ours.raw" | tr -d '\n'; printf '\n           '
    ffmpeg -nostdin -v error -y -i "$f" -f s16le -acodec pcm_s16le "$b.ref.raw"
    python3 tools/mp3compare.py "$b.ours.raw" "$b.ref.raw" "$ch"
    printf '  player   '; $B/player_test "$f" "$b.player.raw" | tr -d '\n'
    ffmpeg -nostdin -v error -y -i "$f" -ac 2 -f s16le -acodec pcm_s16le "$b.ref2.raw"
    if [ "$ch" = 2 ]; then printf '\n           '; python3 tools/mp3compare.py "$b.player.raw" "$b.ref2.raw" 2; else echo; fi
    printf '  stalling '; $B/player_test "$f" "$b.stall.raw" stall | tr -d '\n'
    if cmp -s "$b.player.raw" "$b.stall.raw"; then echo "  identical"; else echo "  DIFFERENT"; fi
done
