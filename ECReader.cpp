#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define ECREADER_VERSION "2025.11.30"

// PawnIO IOCTL codes (from LibreHardwareMonitor)
#define PAWNIO_DEVICE_TYPE 41394
#define IOCTL_PAWNIO_LOAD_BINARY CTL_CODE(PAWNIO_DEVICE_TYPE, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PAWNIO_EXECUTE CTL_CODE(PAWNIO_DEVICE_TYPE, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FN_NAME_LENGTH 32

static bool g_verbose = false;

// EC ports and flags
#define EC_DATA_PORT    0x62
#define EC_CMD_PORT     0x66
#define EC_IBF  0x02
#define EC_OBF  0x01

// Safety constants
#define MUTEX_TIMEOUT_MS      1000
#define MUTEX_RETRY_COUNT     3
#define MUTEX_RETRY_DELAY_MS  100
#define MIN_INTERVAL_MS       2000  // Minimum 2 seconds

// Performance optimization constants
#define EC_WAIT_TIMEOUT_MS        20    // Reduced from 100ms
#define EC_BUSY_WAIT_ITERATIONS   100   // ~1-2ms tight polling
#define EC_MAX_RETRIES            3     // Retry attempts

// No longer needed - we use direct DeviceIoControl

class ECReader {
private:
    HANDLE hDriver;
    HANDLE hMutex;
    bool verboseMode;

public:
    bool suppressVerbose;

private:
    int mutexWaitFailures;
    int mutexRetries;
    int successfulReads;
    int failedReads;
    int retryCount;  // Track retry attempts

    bool AcquireMutex() {
        if (hMutex == NULL) {
            if (verboseMode) printf("Warning: No mutex available\n");
            return true;
        }

        for (int retry = 0; retry < MUTEX_RETRY_COUNT; retry++) {
            DWORD waitResult = WaitForSingleObject(hMutex, MUTEX_TIMEOUT_MS);
            
            if (waitResult == WAIT_OBJECT_0) {
                if (verboseMode && retry > 0) printf("Mutex acquired after %d retries\n", retry);
                if (retry > 0) mutexRetries++;
                return true;
            }
            else if (waitResult == WAIT_ABANDONED) {
                if (verboseMode) printf("Warning: Mutex was abandoned\n");
                return true;
            }
            else if (waitResult == WAIT_TIMEOUT) {
                if (verboseMode) printf("Mutex timeout (attempt %d/%d)\n", retry + 1, MUTEX_RETRY_COUNT);
                if (retry < MUTEX_RETRY_COUNT - 1) Sleep(MUTEX_RETRY_DELAY_MS);
            }
            else {
                if (verboseMode) printf("Mutex wait failed: %lu\n", GetLastError());
                break;
            }
        }
        
        mutexWaitFailures++;
        return false;
    }

    void ReleaseMutexSafe() {
        if (hMutex != NULL) ReleaseMutex(hMutex);
    }

public:
    ECReader() : hDriver(INVALID_HANDLE_VALUE), hMutex(NULL), verboseMode(false),
                 suppressVerbose(false), mutexWaitFailures(0), mutexRetries(0),
                 successfulReads(0), failedReads(0), retryCount(0) {}

    void SetVerbose(bool verbose) {
        verboseMode = verbose;
        g_verbose = verbose;
    }

    bool Open() {
        // Open PawnIO driver directly
        hDriver = CreateFileA("\\\\.\\PawnIO",
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              NULL,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              NULL);

        if (hDriver == INVALID_HANDLE_VALUE) {
            printf("Error: Failed to open PawnIO driver. (Error: %lu)\n", GetLastError());
            printf("Possible causes:\n");
            printf("1. PawnIO driver not installed. Get from https://pawnio.eu \n");
            printf("2. Not running as Administrator\n");
            printf("3. Driver service not started. Run 'sc query pawnio' to validate.\n");
            return false;
        }

        if (verboseMode) printf("[Verbose] PawnIO driver opened successfully\n");

        // Load LpcACPIEC module
        if (verboseMode) printf("[Verbose] Testing LpcACPIEC.bin module load...\n");
        if (!LoadModule("LpcACPIEC.bin")) {
            printf("Error: Failed to load LpcACPIEC.bin module\n");
            CloseHandle(hDriver);
            return false;
        }

        if (verboseMode) printf("[Verbose] LpcACPIEC.bin loaded successfully!\n");

        // Open the EC mutex
        hMutex = OpenMutexA(SYNCHRONIZE, FALSE, "Access_EC");
        if (hMutex == NULL) {
            hMutex = OpenMutexA(SYNCHRONIZE, FALSE, "Global\\Access_EC");
        }

        if (hMutex == NULL) {
            if (verboseMode) printf("[Verbose] Warning: Access_EC mutex not found (Error: %lu), continuing without sync\n", GetLastError());
        } else {
            if (verboseMode) printf("[Verbose] EC mutex opened\n");
        }

        return true;
    }

    bool LoadModule(const char* filename) {
        // Get executable directory and construct full path
        char exePath[MAX_PATH];
        char fullPath[MAX_PATH];

        if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
            if (verboseMode) printf("[Verbose] Warning: Failed to get exe path, trying relative: %s\n", filename);
            strncpy_s(fullPath, MAX_PATH, filename, _TRUNCATE);
        } else {
            // Extract directory by finding last backslash
            char* lastSlash = strrchr(exePath, '\\');
            if (lastSlash != NULL) {
                *lastSlash = '\0';  // Truncate at last backslash
                snprintf(fullPath, MAX_PATH, "%s\\%s", exePath, filename);
            } else {
                strncpy_s(fullPath, MAX_PATH, filename, _TRUNCATE);
            }
        }

        if (verboseMode) printf("[Verbose] Loading module: %s\n", fullPath);

        FILE* f = NULL;
        if (fopen_s(&f, fullPath, "rb") != 0 || f == NULL) {
            return false;
        }

        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (fileSize <= 0 || fileSize > 1024 * 1024) {
            fclose(f);
            return false;
        }

        if (verboseMode) printf("[Verbose] Module file size: %ld bytes\n", fileSize);

        BYTE* buffer = (BYTE*)malloc(fileSize);
        if (!buffer) {
            fclose(f);
            return false;
        }

        size_t bytesRead = fread(buffer, 1, fileSize, f);
        fclose(f);

        if (bytesRead != (size_t)fileSize) {
            free(buffer);
            return false;
        }

        // Use DeviceIoControl to load module
        DWORD bytesReturned = 0;
        BOOL result = DeviceIoControl(hDriver,
                                      IOCTL_PAWNIO_LOAD_BINARY,
                                      buffer,
                                      fileSize,
                                      NULL,
                                      0,
                                      &bytesReturned,
                                      NULL);
        free(buffer);

        if (!result) {
            if (verboseMode) printf("[Verbose] DeviceIoControl LOAD_BINARY failed (Error: %lu)\n", GetLastError());
            return false;
        }

        return true;
    }

    void Close() {
        if (hMutex != NULL) {
            CloseHandle(hMutex);
            hMutex = NULL;
        }
        if (hDriver != INVALID_HANDLE_VALUE) {
            CloseHandle(hDriver);
            hDriver = INVALID_HANDLE_VALUE;
        }
    }

    // Execute a module function (like LibreHardwareMonitor does)
    bool Execute(const char* functionName, LONG64* input, int inputCount, LONG64* output, int outputCount) {
        // Build input buffer: 32-byte function name + input data
        int inputBufferSize = FN_NAME_LENGTH + (inputCount * sizeof(LONG64));
        BYTE* inputBuffer = (BYTE*)malloc(inputBufferSize);
        if (!inputBuffer) return false;

        // Clear and copy function name (max 32 chars)
        memset(inputBuffer, 0, FN_NAME_LENGTH);
        strncpy_s((char*)inputBuffer, FN_NAME_LENGTH, functionName, _TRUNCATE);

        // Copy input data after function name
        if (inputCount > 0 && input != NULL) {
            memcpy(inputBuffer + FN_NAME_LENGTH, input, inputCount * sizeof(LONG64));
        }

        // Prepare output buffer
        int outputBufferSize = outputCount * sizeof(LONG64);
        BYTE* outputBuffer = (BYTE*)malloc(outputBufferSize);
        if (!outputBuffer) {
            free(inputBuffer);
            return false;
        }
        memset(outputBuffer, 0, outputBufferSize);

        // Call DeviceIoControl
        DWORD bytesReturned = 0;
        BOOL result = DeviceIoControl(hDriver,
                                      IOCTL_PAWNIO_EXECUTE,
                                      inputBuffer,
                                      inputBufferSize,
                                      outputBuffer,
                                      outputBufferSize,
                                      &bytesReturned,
                                      NULL);

        // Copy output data
        if (result && output != NULL && bytesReturned > 0) {
            DWORD copySize = (bytesReturned < (DWORD)outputBufferSize) ? bytesReturned : (DWORD)outputBufferSize;
            memcpy(output, outputBuffer, copySize);
        }

        free(inputBuffer);
        free(outputBuffer);

        if (!result && verboseMode) {
            printf("[Verbose] DeviceIoControl EXECUTE failed (Error: %lu)\n", GetLastError());
        }

        return result != FALSE;
    }

    // Low-level port I/O using LpcACPIEC module functions
    bool PortRead(USHORT port, UCHAR* value) {
        LONG64 input[1] = { port };
        LONG64 output[1] = { 0 };
        bool result = Execute("ioctl_pio_read", input, 1, output, 1);

        if (!result) {
            if (verboseMode && !suppressVerbose) printf("[Verbose] PortRead(0x%02X) FAILED\n", port);
            return false;
        }

        *value = (UCHAR)output[0];
        if (verboseMode && !suppressVerbose) printf("[Verbose] PortRead(0x%02X) = 0x%02X\n", port, *value);
        return true;
    }

    bool PortWrite(USHORT port, UCHAR value) {
        LONG64 input[2] = { port, value };
        bool result = Execute("ioctl_pio_write", input, 2, NULL, 0);

        if (!result) {
            if (verboseMode && !suppressVerbose) printf("[Verbose] PortWrite(0x%02X, 0x%02X) FAILED\n", port, value);
            return false;
        }

        if (verboseMode && !suppressVerbose) printf("[Verbose] PortWrite(0x%02X, 0x%02X)\n", port, value);
        return true;
    }

    // Wait for EC Input Buffer to be empty (IBF=0)
    bool WaitECReady(int timeoutMs = EC_WAIT_TIMEOUT_MS) {
        DWORD startTime = GetTickCount();
        int iterations = 0;
        bool prevSuppress = suppressVerbose;
        suppressVerbose = true;

        while (GetTickCount() - startTime < (DWORD)timeoutMs) {
            UCHAR status;
            if (!PortRead(EC_CMD_PORT, &status)) {
                suppressVerbose = prevSuppress;
                return false;
            }

            if ((status & EC_IBF) == 0) {
                suppressVerbose = prevSuppress;
                return true;
            }

            iterations++;
            if (iterations > EC_BUSY_WAIT_ITERATIONS) {
                Sleep(0);
            }
        }
        suppressVerbose = prevSuppress;
        if (verboseMode) printf("[Verbose] WaitECReady timeout after %dms\n", timeoutMs);
        return false;
    }

    // Wait for EC Output Buffer to be full (OBF=1)
    bool WaitECOBF(int timeoutMs = EC_WAIT_TIMEOUT_MS) {
        DWORD startTime = GetTickCount();
        int iterations = 0;
        bool prevSuppress = suppressVerbose;
        suppressVerbose = true;

        while (GetTickCount() - startTime < (DWORD)timeoutMs) {
            UCHAR status;
            if (!PortRead(EC_CMD_PORT, &status)) {
                suppressVerbose = prevSuppress;
                return false;
            }

            if ((status & EC_OBF) != 0) {
                suppressVerbose = prevSuppress;
                return true;
            }

            iterations++;
            if (iterations > EC_BUSY_WAIT_ITERATIONS) {
                Sleep(0);
            }
        }
        suppressVerbose = prevSuppress;
        if (verboseMode) printf("[Verbose] WaitECOBF timeout after %dms\n", timeoutMs);
        return false;
    }

    UCHAR ReadECRegister(UCHAR reg, bool* success = NULL) {
        if (success) *success = false;

        // Retry loop for improved reliability
        for (int attempt = 0; attempt < EC_MAX_RETRIES; attempt++) {
            if (!AcquireMutex()) {
                if (attempt < EC_MAX_RETRIES - 1) {
                    retryCount++;
                    if (verboseMode) printf("[Verbose] Mutex acquisition failed, retry %d/%d\n", attempt + 1, EC_MAX_RETRIES - 1);
                    Sleep(0);  // Brief yield before retry
                    continue;
                }
                failedReads++;
                return 0xFF;
            }

            bool ok = true;

            // EC Read Protocol:
            // 1. Wait for IBF=0 (EC ready)
            // 2. Write 0x80 (read command) to command port (0x66)
            // 3. Wait for IBF=0
            // 4. Write register address to data port (0x62)
            // 5. Wait for OBF=1 (data ready)
            // 6. Read data from data port (0x62)

            if (verboseMode) {
                printf("[Verbose] Reading EC register 0x%02X\n", reg);
            }

            // Step 1: Wait for EC to be ready
            if (!WaitECReady()) {
                if (verboseMode) printf("[Verbose] EC not ready before command\n");
                ok = false;
            }

            // Step 2: Send read command (0x80)
            if (ok && !PortWrite(EC_CMD_PORT, 0x80)) {
                if (verboseMode) printf("[Verbose] Failed to write read command\n");
                ok = false;
            }

            // Step 3: Wait for EC to accept command
            if (ok && !WaitECReady()) {
                if (verboseMode) printf("[Verbose] EC not ready after command\n");
                ok = false;
            }

            // Steps 4-6: Critical timing section - suppress verbose to prevent printf delays
            bool prevSuppress = suppressVerbose;
            suppressVerbose = true;

            UCHAR value = 0xFF;
            if (ok) ok = PortWrite(EC_DATA_PORT, reg);      // Step 4: Write register address
            if (ok) ok = WaitECOBF();                       // Step 5: Wait for data ready
            if (ok) ok = PortRead(EC_DATA_PORT, &value);    // Step 6: Read data

            suppressVerbose = prevSuppress;

            if (!ok && verboseMode) {
                printf("[Verbose] EC read sequence failed\n");
            }

            ReleaseMutexSafe();

            if (ok) {
                // Success!
                if (attempt > 0 && verboseMode) {
                    printf("[Verbose] Read succeeded on retry %d\n", attempt);
                }
                if (verboseMode) {
                    printf("[Verbose] EC[0x%02X] = 0x%02X\n", reg, value);
                }
                successfulReads++;
                if (success) *success = true;
                return value;
            }

            // Failed - retry if attempts remaining
            if (attempt < EC_MAX_RETRIES - 1) {
                retryCount++;
                if (verboseMode) {
                    printf("[Verbose] Read failed, retry %d/%d\n", attempt + 1, EC_MAX_RETRIES - 1);
                }
                Sleep(0);  // Brief yield before retry
            }
        }

        // All retries exhausted
        failedReads++;
        return 0xFF;
    }

    // Monitor mode - track changes across all registers in grid format
    void Monitor(int intervalMs, bool useDecimal) {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
        GetConsoleScreenBufferInfo(hConsole, &consoleInfo);
        WORD savedAttributes = consoleInfo.wAttributes;

        UCHAR currentValues[256];
        UCHAR previousValues[256];
        int changeCount = 0;

        memset(currentValues, 0, sizeof(currentValues));
        memset(previousValues, 0, sizeof(previousValues));

        while (true) {
            DWORD readStartTime = GetTickCount();

            // Read all registers
            for (int i = 0; i < 256; i++) {
                bool success;
                currentValues[i] = ReadECRegister((UCHAR)i, &success);
                if (!success) currentValues[i] = 0xFF;
            }

            DWORD readDuration = GetTickCount() - readStartTime;

            // Count changes
            changeCount = 0;
            for (int i = 0; i < 256; i++) {
                if (currentValues[i] != previousValues[i]) {
                    changeCount++;
                }
            }

            // Clear screen and redraw
            system("cls");
            COORD coordScreen = { 0, 0 };
            SetConsoleCursorPosition(hConsole, coordScreen);

            printf("EC Register Monitor (16x16 grid) - Updates every %d seconds        \n", intervalMs / 1000);
            printf("Press Ctrl+C to exit\n");
            printf("Red=changed, Green=non-zero unchanged, Gray=zero/empty\n");
            printf("Changes detected: %d | Read time: %lums\n", changeCount, readDuration);
            printf("=======================================================\n");
            printf("\n");

            // Header
            printf("     ");
            for (int col = 0; col < 16; col++) {
                printf("+%X ", col);
            }
            printf("\n");

            // Grid rows
            for (int row = 0; row < 16; row++) {
                printf("%X0:  ", row);

                for (int col = 0; col < 16; col++) {
                    int index = row * 16 + col;
                    UCHAR value = currentValues[index];
                    bool changed = (currentValues[index] != previousValues[index]);

                    if (changed) {
                        // Red for changed values
                        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
                    } else if (value != 0) {
                        // Green for non-zero unchanged values
                        SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
                    } else {
                        // Dark gray for zero values
                        SetConsoleTextAttribute(hConsole, FOREGROUND_INTENSITY);
                    }

                    printf("%02X ", value);
                    SetConsoleTextAttribute(hConsole, savedAttributes);
                }
                printf("\n");
            }

            // Clear any remaining lines
            for (int i = 0; i < 5; i++) {
                printf("                                                                  \n");
            }

            // Copy current to previous
            memcpy(previousValues, currentValues, sizeof(currentValues));

            // Sleep for remaining time (subtract read duration from interval)
            DWORD sleepTime = (readDuration < (DWORD)intervalMs) ? (intervalMs - readDuration) : 0;
            if (sleepTime > 0) {
                Sleep(sleepTime);
            }
        }
    }

    // Dump all registers in grid format
    void DumpGrid(bool useDecimal) {
        // Get console handle for colors
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
        GetConsoleScreenBufferInfo(hConsole, &consoleInfo);
        WORD savedAttributes = consoleInfo.wAttributes;

        printf("EC Register Dump (16x16 Grid)\n");
        printf("Red = Non-zero values, Gray = Zero/Empty\n");
        printf("=======================================================\n\n");

        // Header
        printf("     ");
        for (int col = 0; col < 16; col++) {
            if (useDecimal) {
                printf("+%2X ", col);
            } else {
                printf("+%X ", col);
            }
        }
        printf("\n");

        // Read and display all registers
        for (int row = 0; row < 16; row++) {
            printf("%X0:  ", row);

            for (int col = 0; col < 16; col++) {
                int index = row * 16 + col;
                bool success;
                UCHAR value = ReadECRegister((UCHAR)index, &success);

                if (success) {
                    // Red for non-zero values, Dark gray for zero
                    if (value != 0) {
                        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
                    } else {
                        SetConsoleTextAttribute(hConsole, FOREGROUND_INTENSITY);
                    }

                    if (useDecimal) {
                        printf("%3d ", value);
                    } else {
                        printf("%02X ", value);
                    }

                    SetConsoleTextAttribute(hConsole, savedAttributes);
                } else {
                    if (useDecimal) {
                        printf(" ?? ");
                    } else {
                        printf("?? ");
                    }
                }
            }
            printf("\n");
        }
        printf("\n");
    }

    void PrintStatistics() {
        printf("\n=== Statistics ===\n");
        printf("Successful reads: %d\n", successfulReads);
        printf("Failed reads:     %d\n", failedReads);
        printf("Retry attempts:   %d\n", retryCount);
        if (hMutex != NULL) {
            printf("Mutex retries:    %d\n", mutexRetries);
            printf("Mutex failures:   %d\n", mutexWaitFailures);
        }
        if (successfulReads + failedReads > 0) {
            float rate = (float)successfulReads / (successfulReads + failedReads) * 100.0f;
            printf("Success rate:     %.1f%%\n", rate);
            if (retryCount > 0) {
                float retryRate = (float)retryCount / (successfulReads + failedReads);
                printf("Avg retries:      %.2f per operation\n", retryRate);
            }
        }
        printf("==================\n");
    }
};

void PrintUsage(const char* programName) {
    printf("EC Register Reader - READ-ONLY Tool\n");
	printf("PawnIO Driver Must be Installed. Admin Privilege Required!\n");
    printf("====================================\n\n");
    printf("Usage:\n");
    printf("  %s <command> [options]\n\n", programName);
    
    printf("Commands:\n");
    printf("  monitor                - Monitor all registers, show changes\n");
    printf("  -r <reg> [reg2...]     - Read specific register(s)\n");
    printf("  dump                   - Dump all registers in grid format\n");
    printf("  version                - Show version information\n");
    printf("  -h, --help             - Show this help\n\n");
    
    printf("Options:\n");
    printf("  -i <seconds>           - Update interval for monitor (default: 5, min: 2)\n");
    printf("  -d                     - Display values in decimal instead of hex\n");
    printf("  -v                     - Verbose mode (for -r command only)\n");
    printf("  -s                     - Show statistics after operation\n\n");
    
    printf("Examples:\n");
    printf("  %s monitor             - Monitor with 5 second updates\n", programName);
    printf("  %s monitor -i 3        - Monitor with 3 second updates\n", programName);
    printf("  %s monitor -d          - Monitor showing decimal values\n", programName);
    printf("  %s -r 30               - Read register 0x30\n", programName);
    printf("  %s -r 30 31 32         - Read multiple registers\n", programName);
    printf("  %s -r 30 -v            - Read with verbose debug output\n", programName);
    printf("  %s -r 30 -d            - Read register 0x30 in decimal\n", programName);
    printf("  %s dump                - Dump all 256 registers\n", programName);
    printf("  %s dump -d             - Dump in decimal format\n\n", programName);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 0;
    }

    ECReader reader;
    bool verboseMode = false;
    bool showStats = false;
    bool useDecimal = false;
    int intervalSec = 5;
    
    // Parse global flags
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verboseMode = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            showStats = true;
        } else if (strcmp(argv[i], "-d") == 0) {
            useDecimal = true;
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            intervalSec = atoi(argv[i + 1]);
            if (intervalSec < 2) {
                printf("Error: Minimum interval is 2 seconds\n");
                return 1;
            }
            i++; // Skip the interval value
        }
    }
    
    reader.SetVerbose(verboseMode);
    
    // Handle commands
    const char* command = argv[1];
    
    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
        PrintUsage(argv[0]);
        return 0;
    }

    if (strcmp(command, "version") == 0) {
        printf("ECReader v%s\n", ECREADER_VERSION);
        return 0;
    }

    // Open driver for all commands except help and version
    if (!reader.Open()) {
        return 1;
    }
    
    if (strcmp(command, "monitor") == 0) {
        reader.suppressVerbose = true;
        reader.Monitor(intervalSec * 1000, useDecimal);
    }
    else if (strcmp(command, "-r") == 0) {
        // Read specific registers
        if (argc < 3) {
            printf("Error: No register address specified\n");
            reader.Close();
            return 1;
        }
        
        // Collect all register addresses
        bool first = true;
        for (int i = 2; i < argc; i++) {
            // Skip flags
            if (argv[i][0] == '-') {
                if (strcmp(argv[i], "-i") == 0) i++; // Skip interval value
                continue;
            }
            
            UCHAR reg = (UCHAR)strtoul(argv[i], NULL, 16);
            bool success;
            UCHAR value = reader.ReadECRegister(reg, &success);
            
            if (success) {
                if (!first) printf(",");
                if (useDecimal) {
                    printf("0x%02X:%d", reg, value);
                } else {
                    printf("0x%02X:%02X", reg, value);
                }
                first = false;
            } else {
                if (!first) printf(",");
                printf("0x%02X:??", reg);
                first = false;
            }
        }
        printf("\n");
    }
    else if (strcmp(command, "dump") == 0) {
        reader.suppressVerbose = true;
        reader.DumpGrid(useDecimal);
    }
    else {
        printf("Error: Unknown command '%s'\n", command);
        printf("Run '%s --help' for usage\n", argv[0]);
        reader.Close();
        return 1;
    }
    
    if (showStats) {
        reader.PrintStatistics();
    }
    
    reader.Close();
    return 0;
}
