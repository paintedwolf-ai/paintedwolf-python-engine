package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"testing"
)

const (
	pinnedInputSHA256   = "dbd949fbb473d96277cc3fff67fae32b14d713830b339b1687bc9180cbc3c88c"
	pinnedMeteredSHA256 = "3e54c6e508679f660356422f4640f971c9ecf1acc7a3e1754547c6be2153c91a"
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
