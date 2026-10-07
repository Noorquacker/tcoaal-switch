// Host tool: compile global scripts to QuickJS bytecode ("<file>.js" -> "<file>.jsbc").
//   jsbc --root <romfs dir> file.js...
// Function names/line numbers are kept for stack traces; source text is stripped.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    const char *root = NULL;
    int first = 1;
    if (argc > 2 && !strcmp(argv[1], "--root")) {
        root = argv[2];
        first = 3;
    }
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    int failed = 0;
    for (int i = first; i < argc; i++) {
        const char *path = argv[i];
        size_t len;
        char *src = read_all(path, &len);
        if (!src) {
            fprintf(stderr, "jsbc: cannot read %s\n", path);
            failed++;
            continue;
        }
        // name scripts by their virtual path so stack traces match runtime paths
        char name[1024];
        size_t rl = root ? strlen(root) : 0;
        if (root && !strncmp(path, root, rl)) snprintf(name, sizeof name, "/game%s%s", path[rl] == '/' ? "" : "/", path + rl);
        else snprintf(name, sizeof name, "%s", path);
        size_t skip = (len >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) ? 3 : 0;
        JSValue fn = JS_Eval(ctx, src + skip, len - skip, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        free(src);
        if (JS_IsException(fn)) {
            JSValue e = JS_GetException(ctx);
            const char *m = JS_ToCString(ctx, e);
            fprintf(stderr, "jsbc: %s: %s\n", path, m);
            JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, e);
            failed++;
            continue;
        }
        size_t outlen;
        uint8_t *out = JS_WriteObject(ctx, &outlen, fn, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE);
        JS_FreeValue(ctx, fn);
        char outpath[1100];
        snprintf(outpath, sizeof outpath, "%sbc", path);
        FILE *f = fopen(outpath, "wb");
        if (!f || fwrite(out, 1, outlen, f) != outlen) {
            fprintf(stderr, "jsbc: cannot write %s\n", outpath);
            failed++;
        }
        if (f) fclose(f);
        js_free(ctx, out);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return failed ? 1 : 0;
}
