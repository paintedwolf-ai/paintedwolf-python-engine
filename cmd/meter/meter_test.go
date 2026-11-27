package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"testing"
)

const (
	pinnedInputSHA256   = "6b3ae6169391f6a59a52bdac5bd3198833abf7e1d9ae1e2d38c6861c3ec6a137"
	pinnedMeteredSHA256 = "d90c0b178b9e18130a4edb2880e5334daa04aa3da53080938c523e0dfaceff79"
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
