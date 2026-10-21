// SPDX-License-Identifier: GPL-2.0-or-later
package com.mateussouza.emuorbit.advance.emulator.psp;

import android.view.Surface;
import java.io.IOException;
import java.nio.ByteBuffer;

/** Exact, deliberately small RegisterNatives boundary. No arbitrary library loader. */
final class PspNativeBridge {
    private PspNativeBridge() { }
    static native long create(String library, String media, String system, String saves) throws IOException;
    static native void window(long handle, Surface surface) throws IOException;
    static native int step(long handle, int buttons, short x, short y, ByteBuffer pcm) throws IOException;
    static native boolean pixels(long handle, ByteBuffer rgba) throws IOException;
    static native ByteBuffer capture(long handle) throws IOException;
    static native void release(long handle) throws IOException;
    static native void restore(long handle, ByteBuffer validated) throws IOException;
    static native void reset(long handle) throws IOException;
    static native void pause(long handle) throws IOException;
    static native long[] stats(long handle) throws IOException;
    static native double[] timing(long handle) throws IOException;
    static native void close(long handle) throws IOException;
    static native boolean quiescent();
}
