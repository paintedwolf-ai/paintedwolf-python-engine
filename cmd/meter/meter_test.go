package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"testing"
)

const (
	pinnedInputSHA256   = "850ff3aaac98b0b3126d2c3fef5a04a691bd67c79f4f31e630e3386d3eab3635"
	pinnedMeteredSHA256 = "ef198dd5a41a11849a27598b64a400f8321a5e3427a666e89fa6af93dc7667c9"
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
