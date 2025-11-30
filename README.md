# ECReader

**Read-only** Windows tool for reading Embedded Controller registers via PawnIO driver.

## Features

- **Monitor mode**: Live 16×16 grid showing all 256 registers with change detection
- **Dump mode**: One-time snapshot of all registers
- **Read mode**: Query specific registers for scripting
- **Fast**: Optimized to scan all 256 registers in ~1.5 seconds
- **Safe**: Read-only, mutex-protected, no EC hammering

## Quick Start

**Prerequisites:**
- Windows 10/11
- [PawnIO driver](https://pawnio.eu) installed
- Administrator privileges

**Run:**
```bash
ECReader.exe monitor           # Live grid monitor
ECReader.exe dump              # One-time snapshot
ECReader.exe -r 30 31 32       # Read specific registers
ECReader.exe version           # Show version
```

## Commands

### Monitor Mode
```bash
ECReader.exe monitor              # 5 second updates
ECReader.exe monitor -i 2         # 2 second updates (minimum)
```

Shows 16×16 grid with color coding:
- **Red** = value changed since last scan. First Scan always shows red.
- **Green** = non-zero unchanged value
- **Gray** = zero value

Grid format makes register addresses easy to calculate:
- Row labels: `00:`, `10:`, `20:`, ..., `F0:`
- Column headers: `+0`, `+1`, ..., `+F`
- Register address = row + column (e.g., row `20:` + column `+5` = `0x25`)

### Dump Mode
```bash
ECReader.exe dump                 # Hex values
ECReader.exe dump -d              # Decimal values
```

One-time 16×16 grid snapshot. Red = non-zero, gray = zero.

### Read Mode
```bash
ECReader.exe -r 30                # Single register
ECReader.exe -r 30 31 32          # Multiple registers
ECReader.exe -r 30 -d             # Decimal output
```

Output: `0x30:5A,0x31:3C,0x32:28`

## Options

| Flag | Description |
|------|-------------|
| `-i <seconds>` | Update interval (min: 2, default: 5) |
| `-d` | Decimal instead of hex |
| `-v` | Verbose debug output (for `-r` command only) |
| `-s` | Show statistics |

## Use Cases

**Find CPU temperature register:**
1. Start monitor mode
2. Run stress test
3. Watch for increasing values

**Find fan speed register:**
1. Start monitor with `-i 2`
2. Change fan speed
3. Watch for changing values

**Script automation:**
```powershell
while($true) {
  .\ECReader.exe -r 30
  Start-Sleep 1
}
```

## Performance

- **Full scan**: ~1.5 seconds (256 registers)
- **Per register**: ~6ms average
- **Memory**: ~2MB runtime
- **Optimizations**: Adaptive busy-wait, retry logic, reduced timeouts

## Safety

- ✅ Read-only (no write capability)
- ✅ Mutex synchronization
- ✅ 2-second minimum interval
- ✅ Automatic retry on conflicts
- ✅ Clean error handling

## Troubleshooting

**"Cannot open PawnIO driver"**
```cmd
sc query PawnIO
sc start PawnIO
```
Run as Administrator.

**Monitor shows no changes**
- Close HWiNFO and other monitoring tools
- Try faster updates: `-i 2`
- Use `dump` to see current values

**Timing:** ~6ms per register with optimized waits

## Building

**WSL + MinGW:**
```bash
sudo apt install mingw-w64
./build.sh
```

## License

Educational/utility. Use at your own risk.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY.

## Credits

Built using [PawnIO](https://pawnio.eu) driver for EC access.
