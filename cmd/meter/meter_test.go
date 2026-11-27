package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"testing"
)

const (
	pinnedInputSHA256   = "fd955c1b91ef90a070564314f9bc6c98443bc731c462191ebca2e5a1ec317c8c"
	pinnedMeteredSHA256 = "22d6ad6719bd89707465bcf066b02b411d53ebe710cce056af3ec41cbaca257d"
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
