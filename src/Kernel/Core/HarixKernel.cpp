#include "HarixKernel.h"
#include "../../Runtime/JSBindings.h"
#include "../../FileSystem/FileSystem.h"
#include "../../Settings/TouchDriver.h"
#include "../../UI/UiLayout.h"
#include "../Services/NotificationManager.h"
#include "../Services/IPCManager.h"

duk_context *HarixKernel::ctx = nullptr;
KryonDisplay *HarixKernel::tftInstance = nullptr;

#include <esp_heap_caps.h>

static void *my_alloc(void *udata, duk_size_t size) {
    if (size == 0) return nullptr;
    
    void *p = nullptr;
#if defined(BOARD_HAS_PSRAM)
    if (psramFound()) {
        // Allocate directly from the 8MB Octal PSRAM pool
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
    if (!p) {
        // Fallback to internal SRAM
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = malloc(size);
    }
    if (!p) {
        Serial.printf("[HarixKernel] Out of memory allocating %u bytes! (Free PSRAM: %u, Free Heap: %u)\n", 
                      (unsigned int)size, ESP.getFreePsram(), ESP.getFreeHeap());
    }
    return p;
}

static void *my_realloc(void *udata, void *ptr, duk_size_t size) {
    if (size == 0) {
        if (ptr) free(ptr);
        return nullptr;
    }
    if (!ptr) {
        return my_alloc(udata, size);
    }
    
    void *p = nullptr;
#if defined(BOARD_HAS_PSRAM)
    if (psramFound()) {
        p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
    if (!p) {
        p = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = realloc(ptr, size);
    }
    if (!p) {
        Serial.printf("[HarixKernel] Out of memory reallocating %u bytes! (Free PSRAM: %u, Free Heap: %u)\n", 
                      (unsigned int)size, ESP.getFreePsram(), ESP.getFreeHeap());
    }
    return p;
}

static void my_free(void *udata, void *ptr) {
    if (ptr) {
        free(ptr);
    }
}

static void printToAllSerials(const String& str) {
    Serial.print(str);
    Serial.flush();
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
    Serial0.print(str);
    Serial0.flush();
#endif
}

// ---------------------------------------------------------------------------------------------
// The fatal-error screens below.
//
// Three of them are the same panel -- a red field, "Out Of Ram Error", three lines of advice and a
// white "X" -- spelled out once per failure path, in 240x320 pixels. They are drawn at the UiLayout
// scale now (the backend sets the canvas's text size from it), so those fixed offsets would push a
// doubled glyph height into the next line. Everything here is derived from the metrics instead, once,
// so the copies cannot disagree with each other or with the panel.
//
// The "X" is the only way off these screens, and it runs on a path that has already given up, so its
// tap zone is deliberately generous: the button rectangle itself plus the strip below it that the
// historical `tx >= 200 && ty <= 40` also accepted.
// ---------------------------------------------------------------------------------------------
static void drawOutOfRamPanel(KryonDisplay* tft) {
    if (!tft) return;
    const int16_t s = (int16_t)UiLayout::current().scale;
    tft->fillScreen(TFT_RED);
    tft->setTextColor(TFT_WHITE, TFT_RED);
    tft->drawString("Out Of Ram Error", (int16_t)(10 * s), (int16_t)(20 * s), 4);
    tft->drawString("Please turn off WiFi in", (int16_t)(10 * s), (int16_t)(60 * s), 2);
    tft->drawString("setting to free the ram", (int16_t)(10 * s), (int16_t)(80 * s), 2);
    tft->drawString("and make this app running", (int16_t)(10 * s), (int16_t)(100 * s), 2);
}

static void drawFatalExitButton(KryonDisplay* tft) {
    if (!tft) return;
    const UiMetrics& m = UiLayout::current();
    const UiRect& b = m.appExitButton;
    tft->fillRoundRect(b.x, b.y, b.w, b.h, (int32_t)(5 * m.scale), TFT_WHITE);
    tft->setTextColor(TFT_RED, TFT_WHITE);
    tft->drawString("X", (int16_t)(b.x + 15 * m.scale), (int16_t)(b.y + 8 * m.scale), 2);
}

static void waitForFatalExitTap() {
    const UiMetrics& m = UiLayout::current();
    const UiRect& b = m.appExitButton;
    const int16_t tapBottom = (int16_t)(b.y + b.h + 10 * m.scale);
    uint16_t tx = 0, ty = 0;
    while (true) {
        if (TouchDriver::getTouch(&tx, &ty) && tx >= b.x && ty <= tapBottom) break;
        delay(50);
    }
}

// Dummy fatal error handler if duktape aborts
static void my_fatal(void *udata, const char *msg) {
    String errStr = "\n================================================================================\n";
    errStr += "[KryonOS Duktape Fatal Error] ";
    errStr += (msg ? msg : "no message");
    errStr += "\n================================================================================\n";
    printToAllSerials(errStr);
    
    if (HarixKernel::tftInstance) {
        drawOutOfRamPanel(HarixKernel::tftInstance);

        // Draw an 'X' to close/reboot
        drawFatalExitButton(HarixKernel::tftInstance);
        // The X is the only way off this screen, and this runs inside the main loop that would
        // otherwise flush the frame -- so flush it here. On a canvas backend the button is otherwise
        // invisible, which makes a recoverable error look like a dead board.
        HarixKernel::tftInstance->present();

        // Wait for user to touch the X before rebooting!
        waitForFatalExitTap();
    }
    
    if (msg && strstr(msg, "alloc")) {
        printToAllSerials("out of memory\n");
    }
    ESP.restart(); // Reboot when they close it
}

void HarixKernel::init(KryonDisplay *tft) {
    tftInstance = tft;
    NotificationManager::init();
    IPCManager::init();
    // Duktape heap is no longer initialized here to save 60-80KB of RAM for the WebServer/WiFi.
    // It will be allocated on-demand in runFile() and checkSyntax().
    printToAllSerials("HarixKernel initialized successfully.\n");
}

void HarixKernel::checkJSError(duk_context *ctx, duk_int_t result) {
    if (result != 0) {
        String errName = "Error";
        String errMsg = "";
        String fileName = "";
        int lineNumber = -1;
        String stackTrace = "";
        String fullError = "";

        if (duk_is_error(ctx, -1)) {
            if (duk_get_prop_string(ctx, -1, "name")) {
                errName = duk_safe_to_string(ctx, -1);
            }
            duk_pop(ctx);

            if (duk_get_prop_string(ctx, -1, "message")) {
                errMsg = duk_safe_to_string(ctx, -1);
            }
            duk_pop(ctx);

            if (duk_get_prop_string(ctx, -1, "fileName")) {
                fileName = duk_safe_to_string(ctx, -1);
            }
            duk_pop(ctx);

            if (duk_get_prop_string(ctx, -1, "lineNumber")) {
                lineNumber = duk_get_int(ctx, -1);
            }
            duk_pop(ctx);

            if (duk_get_prop_string(ctx, -1, "stack")) {
                stackTrace = duk_safe_to_string(ctx, -1);
            }
            duk_pop(ctx);

            fullError = duk_safe_to_string(ctx, -1);
        } else {
            fullError = duk_safe_to_string(ctx, -1);
            errMsg = fullError;
        }

        // Intercept hidden OS Exit signal
        if (fullError.indexOf("OS_EXIT") != -1 || errMsg.indexOf("OS_EXIT") != -1) {
            duk_pop(ctx);
            return;
        }

        // Intercept OOM signals
        if (fullError.indexOf("alloc") != -1 || fullError.indexOf("out of memory") != -1 || errMsg.indexOf("out of memory") != -1) {
            String oomReport = "\n================================================================================\n";
            oomReport += "[KryonOS JS Error] OUT OF MEMORY (Heap Exhausted)\n";
            oomReport += "================================================================================\n\n";
            printToAllSerials(oomReport);

            if (tftInstance) {
                drawOutOfRamPanel(tftInstance);

                // Draw an 'X' to close
                drawFatalExitButton(tftInstance);
                tftInstance->present(); // see my_fatal(): the X must be on the panel before we wait

                waitForFatalExitTap();
            }
            duk_pop(ctx);
            return;
        }

        // Output rich formatted error to all Serial Monitors (USB CDC and Hardware UART)
        String report = "\n================================================================================\n";
        report += "[KryonOS JS Exception] " + errName + "\n";
        report += "--------------------------------------------------------------------------------\n";
        if (fileName.length() > 0) {
            report += "File       : " + fileName + "\n";
        }
        if (lineNumber > 0) {
            report += "Line       : " + String(lineNumber) + "\n";
        }
        if (errMsg.length() > 0) {
            report += "Message    : " + errMsg + "\n";
        }
        if (stackTrace.length() > 0) {
            report += "Stack Trace:\n" + stackTrace + "\n";
        } else if (fullError.length() > 0) {
            report += "Details    :\n" + fullError + "\n";
        }
        report += "================================================================================\n\n";

        printToAllSerials(report);

        if (tftInstance) {
            const UiMetrics& m = UiLayout::current();
            const int16_t   s = (int16_t)m.scale;
            const int16_t   x = (int16_t)(10 * s);
            const int16_t   pitch = (int16_t)(18 * s); // clears the 16px body glyph at this scale

            tftInstance->fillScreen(TFT_RED);
            tftInstance->setTextColor(TFT_WHITE, TFT_RED);
            tftInstance->setTextDatum(TL_DATUM);
            tftInstance->drawString("JS EXCEPTION!", x, (int16_t)(10 * s), 4);

            int yPos = 40 * s;
            if (errName.length() > 0) {
                tftInstance->drawString(errName + ":", x, yPos, 2);
                yPos += pitch;
            }
            if (fileName.length() > 0 || lineNumber > 0) {
                String loc = (fileName.length() > 0 ? fileName : "app.js") + ":" + String(lineNumber);
                tftInstance->drawString(loc, x, yPos, 2);
                yPos += pitch;
            }

            // Draw lines of message / stack -- down to the footer, which is where the historical
            // 290 landed on a 320-tall screen.
            String displayStr = (errMsg.length() > 0) ? errMsg : fullError;
            int startIdx = 0;
            while (startIdx < displayStr.length() && yPos < m.footer.y) {
                int nextNewline = displayStr.indexOf('\n', startIdx);
                if (nextNewline == -1) nextNewline = displayStr.length();
                String line = displayStr.substring(startIdx, nextNewline);
                tftInstance->drawString(line, x, yPos, 2);
                yPos += pitch;
                startIdx = nextNewline + 1;
            }

            // Draw an 'X' to close
            drawFatalExitButton(tftInstance);
            tftInstance->present(); // see my_fatal(): the X must be on the panel before we wait

            waitForFatalExitTap();
        }
    }
    duk_pop(ctx); // pop result or error
}

void HarixKernel::executeJS(const char* jsCode) {
    if (!ctx) return;
    
    duk_int_t rc = duk_peval_string(ctx, jsCode);
    checkJSError(ctx, rc);
}

// Struct to pass data to the syntax check task
struct SyntaxCheckParams {
    const char* jsCode;
    String result;
    bool done;
};

static void syntaxCheckTask(void* param) {
    SyntaxCheckParams* p = (SyntaxCheckParams*)param;
    
    duk_context *tempCtx = duk_create_heap(my_alloc, my_realloc, my_free, nullptr, nullptr);
    if (!tempCtx) {
        p->result = "Out of Memory allocating JS heap";
        p->done = true;
        vTaskDelete(NULL);
        return;
    }
    
    duk_int_t rc = duk_pcompile_string(tempCtx, 0, p->jsCode);
    if (rc != 0) {
        String errName = "SyntaxError";
        String errMsg = "";
        int lineNumber = -1;
        String stack = "";

        if (duk_is_error(tempCtx, -1)) {
            if (duk_get_prop_string(tempCtx, -1, "name")) errName = duk_safe_to_string(tempCtx, -1);
            duk_pop(tempCtx);
            if (duk_get_prop_string(tempCtx, -1, "message")) errMsg = duk_safe_to_string(tempCtx, -1);
            duk_pop(tempCtx);
            if (duk_get_prop_string(tempCtx, -1, "lineNumber")) lineNumber = duk_get_int(tempCtx, -1);
            duk_pop(tempCtx);
            if (duk_get_prop_string(tempCtx, -1, "stack")) stack = duk_safe_to_string(tempCtx, -1);
            duk_pop(tempCtx);
            p->result = duk_safe_to_string(tempCtx, -1);
        } else {
            p->result = duk_safe_to_string(tempCtx, -1);
            errMsg = p->result;
        }

        String synReport = "\n================================================================================\n";
        synReport += "[KryonOS JS Syntax Error] " + errName + "\n";
        synReport += "--------------------------------------------------------------------------------\n";
        if (lineNumber > 0) {
            synReport += "Line       : " + String(lineNumber) + "\n";
        }
        if (errMsg.length() > 0) {
            synReport += "Message    : " + errMsg + "\n";
        }
        if (stack.length() > 0) {
            synReport += "Stack Trace:\n" + stack + "\n";
        }
        synReport += "================================================================================\n\n";
        printToAllSerials(synReport);
    } else {
        p->result = "";
    }
    duk_pop(tempCtx);
    duk_destroy_heap(tempCtx);
    
    p->done = true;
    vTaskDelete(NULL);
}

String HarixKernel::checkSyntax(const char* jsCode) {
    SyntaxCheckParams params;
    params.jsCode = jsCode;
    params.result = "";
    params.done = false;
    
    // Run in a dedicated task with 16KB stack to avoid overflowing loopTask
    BaseType_t created = xTaskCreatePinnedToCore(
        syntaxCheckTask,
        "syntaxChk",
        16384,        // 16KB stack just for this task
        &params,
        1,            // Low priority
        NULL,
        1             // Run on Core 1
    );
    
    if (created != pdPASS) {
        return "Failed to create syntax check task";
    }
    
    // Block until the task finishes
    while (!params.done) {
        delay(10);
    }
    
    return params.result;
}

void HarixKernel::runFile(const char* filePath) {
    if (ctx) {
        duk_destroy_heap(ctx);
        ctx = nullptr;
    }

    ctx = duk_create_heap(my_alloc, my_realloc, my_free, nullptr, my_fatal);
    if (!ctx) {
        printToAllSerials("Failed to create Duktape heap for app.\n");
        if (tftInstance) {
            drawOutOfRamPanel(tftInstance);

            // Draw an 'X' to close
            drawFatalExitButton(tftInstance);
            tftInstance->present(); // see my_fatal(): the X must be on the panel before we wait

            waitForFatalExitTap();
        }
        return; // Soft exit back to OS
    }

    JSBindings::init(ctx, tftInstance);
    
    // Configure strict app filesystem sandbox root
    {
        String pathStr = String(filePath);
        int lastSlash = pathStr.lastIndexOf('/');
        if (lastSlash >= 0) {
            String appDir = pathStr.substring(0, lastSlash + 1);
            JSBindings::setSandboxRoot(appDir);
        } else {
            JSBindings::setSandboxRoot("");
        }
    }

    {
        String content = FileSystem::readTextFile(filePath);
        if (content.length() == 0) {
            String err = "Failed to read JS file: " + String(filePath) + "\n";
            printToAllSerials(err);
            JSBindings::setSandboxRoot("");
            duk_destroy_heap(ctx);
            ctx = nullptr;
            return;
        }

        duk_push_string(ctx, filePath);
        duk_int_t rc = duk_pcompile_string_filename(ctx, 0, content.c_str());
        if (rc != 0) {
            checkJSError(ctx, rc);
            JSBindings::setSandboxRoot("");
            duk_destroy_heap(ctx);
            ctx = nullptr;
            return;
        }
    } // `content` String is destroyed here, freeing 50KB+ of RAM before the app runs

    duk_int_t rc = duk_pcall(ctx, 0);
    checkJSError(ctx, rc);
    
    // Cleanup active HTTP servers, sockets, sandbox root, and JS callbacks
    JSBindings::cleanup(ctx);
    JSBindings::setSandboxRoot("");

    // Destroy heap after app exits to free RAM
    duk_destroy_heap(ctx);
    ctx = nullptr;
}

void HarixKernel::loop() {
    
}
