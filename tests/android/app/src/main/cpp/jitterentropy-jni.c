/*
 * JNI binding of the Jitter RNG for the example app
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jitterentropy.h"

/*
 * The handle Kotlin holds: jent_read_entropy_safe() may replace the
 * collector, and the pointer inside follows the replacement.
 */
struct jent_jni_handle {
	struct rand_data *ec;
};

static struct jent_jni_handle *jent_jni_handle(jlong handle)
{
	return (struct jent_jni_handle *)(intptr_t)handle;
}

/* Wipe a buffer that held output; a plain memset may be elided. */
static void jent_jni_wipe(void *buf, size_t len)
{
	volatile unsigned char *p = buf;

	while (len--)
		*p++ = 0;
}

JNIEXPORT jint JNICALL
Java_de_chronox_jitterentropy_example_JitterEntropy_nativeVersion(
	JNIEnv *env, jclass cls)
{
	(void)env;
	(void)cls;

	return (jint)jent_version();
}

/* The flags JitterEntropy.Config passes, spelled out on the Kotlin side. */
_Static_assert(JENT_FORCE_FIPS == (1 << 5), "JitterEntropy.Mode.FIPS");
_Static_assert(JENT_NTG1 == (1 << 6), "JitterEntropy.Mode.NTG1");
_Static_assert(JENT_FORCE_INTERNAL_TIMER == (1 << 3),
	       "JitterEntropy.Config.timerThread");
_Static_assert(JENT_FLAGS_TO_MEMSIZE_SHIFT == 27 &&
	       JENT_MAX_MEMSIZE_MAX == JENT_MAX_MEMSIZE_TO_FLAGS(20),
	       "JitterEntropy.Config.memSize");
_Static_assert(JENT_FLAGS_TO_HASHLOOP_SHIFT == 24 &&
	       JENT_MAX_HASHLOOP == JENT_HASHLOOP_TO_FLAGS(7),
	       "JitterEntropy.Config.hashLoop");

JNIEXPORT jint JNICALL
Java_de_chronox_jitterentropy_example_JitterEntropy_nativeInit(
	JNIEnv *env, jclass cls, jint osr, jint flags)
{
	(void)env;
	(void)cls;

	return jent_entropy_init_ex((unsigned int)osr, (unsigned int)flags);
}

JNIEXPORT jlong JNICALL
Java_de_chronox_jitterentropy_example_JitterEntropy_nativeAlloc(
	JNIEnv *env, jclass cls, jint osr, jint flags)
{
	struct jent_jni_handle *h;

	(void)env;
	(void)cls;

	h = calloc(1, sizeof(*h));
	if (!h)
		return 0;

	/* With the rate the power-on tests ran with. */
	h->ec = jent_entropy_collector_alloc((unsigned int)osr,
					     (unsigned int)flags);
	if (!h->ec) {
		free(h);
		return 0;
	}

	return (jlong)(intptr_t)h;
}

JNIEXPORT void JNICALL
Java_de_chronox_jitterentropy_example_JitterEntropy_nativeFree(
	JNIEnv *env, jclass cls, jlong handle)
{
	struct jent_jni_handle *h = jent_jni_handle(handle);

	(void)env;
	(void)cls;

	if (!h)
		return;

	jent_entropy_collector_free(h->ec);
	free(h);
}

/*
 * Fill out with random bytes. Returns 0, or the negative JENT_ERR_* code of
 * the read that failed - the array then holds no output.
 *
 * Chunked through a stack buffer: pinning the array for the milliseconds a
 * chunk takes would stall the garbage collector.
 */
JNIEXPORT jint JNICALL
Java_de_chronox_jitterentropy_example_JitterEntropy_nativeRead(
	JNIEnv *env, jclass cls, jlong handle, jbyteArray out)
{
	struct jent_jni_handle *h = jent_jni_handle(handle);
	char buf[64];
	jsize len = (*env)->GetArrayLength(env, out);
	jsize done = 0;
	ssize_t ret = 0;

	(void)cls;

	if (!h)
		return JENT_ERR_EINVAL;

	while (done < len) {
		size_t todo = (size_t)(len - done);

		if (todo > sizeof(buf))
			todo = sizeof(buf);

		ret = jent_read_entropy_safe(&h->ec, buf, todo);
		if (ret < 0)
			break;

		(*env)->SetByteArrayRegion(env, out, done, (jsize)todo,
					   (const jbyte *)buf);
		done += (jsize)todo;
		ret = 0;
	}

	jent_jni_wipe(buf, sizeof(buf));

	if (ret < 0) {
		/* Do not leave a partial result in the caller's array. */
		jbyte *elems = (*env)->GetByteArrayElements(env, out, NULL);

		if (elems) {
			jent_jni_wipe(elems, (size_t)len);
			(*env)->ReleaseByteArrayElements(env, out, elems, 0);
		}
		return (jint)ret;
	}

	return 0;
}

/* The JSON status document of the collector. */
JNIEXPORT jstring JNICALL
Java_de_chronox_jitterentropy_example_JitterEntropy_nativeStatus(
	JNIEnv *env, jclass cls, jlong handle)
{
	struct jent_jni_handle *h = jent_jni_handle(handle);
	/* The size the library documents as holding the whole document. */
	char buf[4096];

	(void)cls;

	if (!h || jent_status(h->ec, buf, sizeof(buf)))
		return NULL;

	return (*env)->NewStringUTF(env, buf);
}
