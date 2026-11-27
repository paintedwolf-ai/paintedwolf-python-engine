package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"testing"
)

const (
	pinnedInputSHA256   = "5ce0cbeb843e6e5abf2d50c7158002e8333c26a40fbe27a7a52e66ee48cf64a8"
	pinnedMeteredSHA256 = "774a08a7863c1dee1b2f21bb80fb9964f0dd5dc27bdf2294bdd8a11934e893c6"
)

func TestInstrumentWasmFuelByteEquivalence(t *testing.T) {
	wasmPath := os.Getenv("PW_PY07_CPYTHON_WASM")
	if wasmPath == "" {
		t.Skip("PW_PY07_CPYTHON_WASM not set")
	}
	raw, err := os.ReadFile(wasmPath)
	if err != nil {
		t.Fatalf("read %s: %v", wasmPath, err)
	}
	inSum := sha256.Sum256(raw)
	if got := hex.EncodeToString(inSum[:]); got != pinnedInputSHA256 {
		t.Fatalf("input python.wasm sha256 = %s, want %s", got, pinnedInputSHA256)
	}
	metered, err := InstrumentWasmFuel(raw)
	if err != nil {
		t.Fatalf("InstrumentWasmFuel: %v", err)
	}
	outSum := sha256.Sum256(metered)
	if got := hex.EncodeToString(outSum[:]); got != pinnedMeteredSHA256 {
		t.Fatalf("metered.wasm sha256 = %s, want %s", got, pinnedMeteredSHA256)
	}
}
