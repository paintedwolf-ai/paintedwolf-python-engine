package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
)

func main() {
	if len(os.Args) < 3 || len(os.Args) > 4 {
		fmt.Fprintf(os.Stderr, "usage: meter <in.wasm> <out.wasm> [expected_sha256]\n")
		os.Exit(2)
	}
	inBytes, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintf(os.Stderr, "read %s: %v\n", os.Args[1], err)
		os.Exit(1)
	}
	outBytes, err := InstrumentWasmFuel(inBytes)
	if err != nil {
		fmt.Fprintf(os.Stderr, "instrument wasm fuel: %v\n", err)
		os.Exit(1)
	}
	sum := sha256.Sum256(outBytes)
	gotHex := hex.EncodeToString(sum[:])
	if len(os.Args) == 4 && os.Args[3] != "" && gotHex != os.Args[3] {
		fmt.Fprintf(os.Stderr, "metered wasm sha256 mismatch: got %s, want %s\n", gotHex, os.Args[3])
		os.Exit(1)
	}
	if err := os.WriteFile(os.Args[2], outBytes, 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "write %s: %v\n", os.Args[2], err)
		os.Exit(1)
	}
	fmt.Printf("%s  %s\n", gotHex, os.Args[2])
}
