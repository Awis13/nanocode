/*
 * thinking.c — adaptive thinking support detection (CMP-304)
 *
 * Separated from provider.c so tests can link this file alone without
 * dragging in BearSSL or the HTTP client.
 */

#include "provider.h"
#include <string.h>

/*
 * Returns 1 if the model supports Anthropic extended thinking.
 * Currently recognises claude-opus-4* and claude-sonnet-4* families.
 * All other models (older Claude, OpenAI-compatible, Ollama) return 0.
 */
int provider_model_supports_thinking(const char *model)
{
    if (!model)
        return 0;
    return (strncmp(model, "claude-opus-4",    13) == 0 ||
            strncmp(model, "claude-sonnet-4",  15) == 0) ? 1 : 0;
}
