#!/bin/bash
# Test cases for filecrypt.
# Run from the repo folder:  bash tests/test_filecrypt.sh

cd "$(dirname "$0")/.." || exit 1
gcc -Wall -Wextra -DFILECRYPT_STANDALONE -o filecrypt_test filecrypt.c || exit 1
FC=./filecrypt_test
T=$(mktemp -d)
pass=0
fail=0

check() {
    if [ "$1" = "0" ]; then
        echo "PASS: $2"; pass=$((pass + 1))
    else
        echo "FAIL: $2"; fail=$((fail + 1))
    fi
}

# 1. Text file round trip
echo "hello operating systems" > "$T/a.txt"
$FC -e -k secret "$T/a.txt" > /dev/null
$FC -d -k secret -o "$T/a.out" "$T/a.txt.enc" > /dev/null
cmp -s "$T/a.txt" "$T/a.out"; check $? "text file round trip"

# 2. Encrypted file looks different from the original
! grep -q "hello" "$T/a.txt.enc"; check $? "encrypted file hides the text"

# 3. Large binary file round trip (5 MB)
head -c 5000000 /dev/urandom > "$T/big.bin"
$FC -e -k key123 -o "$T/big.enc" "$T/big.bin" > /dev/null
$FC -d -k key123 -o "$T/big.out" "$T/big.enc" > /dev/null
cmp -s "$T/big.bin" "$T/big.out"; check $? "5 MB binary round trip"

# 4. Empty file round trip
: > "$T/empty"
$FC -e -k k -o "$T/empty.enc" "$T/empty" > /dev/null
$FC -d -k k -o "$T/empty.out" "$T/empty.enc" > /dev/null
cmp -s "$T/empty" "$T/empty.out"; check $? "empty file round trip"

# 5. Wrong key is rejected and no output is left behind
$FC -d -k wrong -o "$T/bad.out" "$T/a.txt.enc" > /dev/null
[ $? -eq 1 ] && [ ! -e "$T/bad.out" ]; check $? "wrong key rejected"

# 6. Decrypting a file we didn't make is rejected
$FC -d -k secret -o "$T/x.out" "$T/a.txt" > /dev/null
[ $? -eq 1 ]; check $? "non-filecrypt file rejected"

# 7. Key from standard input (the prompt path)
echo "piped" | $FC -e -o "$T/p.enc" "$T/a.txt" > /dev/null 2>&1
echo "piped" | $FC -d -o "$T/p.out" "$T/p.enc" > /dev/null 2>&1
cmp -s "$T/a.txt" "$T/p.out"; check $? "key read from stdin"

# 8. Default output names (.enc added, then removed)
cp "$T/a.txt" "$T/b.txt"
$FC -e -k z "$T/b.txt" > /dev/null
rm "$T/b.txt"
$FC -d -k z "$T/b.txt.enc" > /dev/null
cmp -s "$T/a.txt" "$T/b.txt"; check $? "default output names"

# 9. Output same as input is refused
$FC -e -k z -o "$T/a.txt" "$T/a.txt" > /dev/null
[ $? -eq 1 ] && cmp -s "$T/a.txt" "$T/a.out"; check $? "same input and output refused"

# 10. Missing file, missing mode, bad option
$FC -e -k z "$T/nope" > /dev/null 2>&1;  [ $? -eq 1 ]; check $? "missing file"
$FC -k z "$T/a.txt" > /dev/null;         [ $? -eq 1 ]; check $? "missing -e or -d"
$FC -x "$T/a.txt" > /dev/null;           [ $? -eq 1 ]; check $? "bad option"

# 11. Ctrl+C on a big file exits 130 and removes the partial output
head -c 1500000000 /dev/zero > "$T/huge.bin" 2>/dev/null
$FC -e -k z -o "$T/huge.enc" "$T/huge.bin" > /dev/null &
pid=$!
sleep 0.5
kill -INT $pid
wait $pid
code=$?
[ $code -eq 130 ] && [ ! -e "$T/huge.enc" ]; check $? "Ctrl+C cleans up (exit $code)"

rm -rf "$T" filecrypt_test
echo "----"
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
