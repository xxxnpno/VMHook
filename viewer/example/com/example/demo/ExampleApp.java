package com.example.demo;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * A long-running example JVM to test the vmhook viewer against.
 *
 * It deliberately exposes a rich surface for the viewer + CLI + MCP: static AND
 * instance fields of every primitive descriptor (Z/C/B/S/I/J/F/D) plus String,
 * arrays and object refs; a {@code Worker} subclass (for inherited-field and
 * live-instance testing); a nested class, an interface and an enum; and a pool
 * of 40 live {@code Worker} instances mutated every second so the live-heap
 * inspector (and `vmhook_cli instances` / `statics`) has changing state to show.
 *
 * Build + run:
 *   javac -d out com/example/demo/*.java
 *   java  -cp out com.example.demo.ExampleApp
 */
public class ExampleApp
{
    // ── static fields (various types) ────────────────────────────────────────
    public static final String        APP_NAME     = "vmhook demo";
    public static       int            tickCounter  = 0;
    private static      double         ratio        = 1.5;
    static              long[]         history      = new long[16];
    static              Map<String, Integer> scores = new HashMap<>();
    protected static    Color          defaultColor = Color.GREEN;
    // extra static primitives to exercise every descriptor in the static-value path
    public static       boolean        debug        = true;
    public static       char           grade        = 'A';
    public static       byte           level        = 7;
    public static       short          port         = 8080;
    public static       long           epoch        = 1_700_000_000_000L;
    public static       float          piF          = 3.14159f;

    // ── instance fields (various types) ──────────────────────────────────────
    public              int            id;
    public              int            ticks;   // mutated every second (live demo)
    private             String         label;
    protected           boolean        active;
    float[]                            samples;
    List<String>                       tags;
    Inner                              inner;
    // extra instance primitives so the instance-value path covers every descriptor
    char                               ch    = 'Z';
    byte                               b8    = 42;
    short                              s16   = 1000;
    long                               lVal  = 9_000_000_000L;
    double                             dVal  = 2.718281828;

    public ExampleApp(int id, String label)
    {
        this.id      = id;
        this.label   = label;
        this.active  = true;
        this.samples = new float[4];
        this.tags    = new ArrayList<>();
        this.inner   = new Inner();
    }

    // ── methods (various signatures) ─────────────────────────────────────────
    public  int     getId()                                    { return id; }
    public  String  getLabel()                                 { return label; }
    public  void    setLabel(String value)                     { this.label = value; }
    public  boolean isActive()                                 { return active; }
    public  double  compute(int a, double b)                   { return a * b + ratio; }
    public  String  describe(int n, String s, boolean flag)    { return label + ":" + n + ":" + s + ":" + flag; }
    public  long[]  snapshot()                                 { return history.clone(); }
    // Object-argument methods — demo passing a SAVED object from the viewer's
    // clipboard as a method argument (grab one Worker's `inner`, then call these).
    public  int     innerValueOf(Inner in)                     { return in == null ? -1 : in.innerValue; }
    public  void    adopt(Inner other)                         { this.inner = other; }
    public  static  void tick()                                { tickCounter++; }
    public  static  int  scoreOf(String who)                   { return scores.getOrDefault(who, 0); }

    // ── nested types ─────────────────────────────────────────────────────────
    static class Inner
    {
        int    innerValue = 42;
        String innerName  = "inner";
        int    doubleIt(int x) { return x * 2; }
    }

    interface Greeter
    {
        String greet(String who);
    }

    enum Color { RED, GREEN, BLUE }

    // A subclass so the instance inspector can show INHERITED fields (id, ticks,
    // label, active, ... all come from ExampleApp) alongside Worker's own (seq, team).
    public static class Worker extends ExampleApp
    {
        public  int    seq;
        private String team;
        Worker(int id, String label, int seq)
        {
            super(id, label);
            this.seq  = seq;
            this.team = "team-" + (seq % 4);
        }
    }

    // Keep many live instances alive (a real heap has more than one) so the
    // viewer's instance inspector has rows to sort / filter — each mutates every tick.
    static final List<Worker> live = new ArrayList<>();

    // ── entry point (runs forever so it stays attachable) ────────────────────
    // A stream of JDK classes not loaded at startup — main() loads one per tick
    // so a vmhook-viewer re-scan surfaces classes added to the JVM AT RUNTIME
    // (each forName also pulls in that class's own dependencies).
    private static final String[] LAZY_CLASSES = {
        "java.util.StringJoiner", "java.util.Base64", "java.util.BitSet",
        "java.util.zip.CRC32", "java.util.zip.Adler32", "java.util.UUID",
        "java.time.Duration", "java.time.Period", "java.time.Year",
        "java.time.MonthDay", "java.time.DayOfWeek", "java.text.DecimalFormat",
        "java.util.regex.Pattern", "java.util.StringTokenizer", "java.math.BigInteger",
        "java.util.Scanner", "java.util.PriorityQueue", "java.util.ArrayDeque",
        "java.util.concurrent.CountDownLatch", "java.util.concurrent.Semaphore",
        "java.util.concurrent.CyclicBarrier", "java.util.concurrent.Phaser",
        "java.util.concurrent.Exchanger", "java.util.concurrent.atomic.LongAdder",
        "java.util.concurrent.atomic.DoubleAdder", "java.util.stream.Collectors",
    };

    // A custom ClassLoader used to DEFINE brand-new classes at runtime — this
    // goes through java.lang.ClassLoader.defineClass, which vmhook's
    // on_class_loaded hook detours, so the viewer sees each one live.
    static final class RuntimeLoader extends ClassLoader
    {
        Class<?> define(String dottedName, byte[] bytes)
        {
            return defineClass(dottedName, bytes, 0, bytes.length);
        }
    }
    private static final RuntimeLoader RUNTIME_LOADER = new RuntimeLoader();

    // Build the minimal valid class file for `public class <internalName> {}`.
    private static byte[] genClass(String internalName) throws Exception
    {
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
        java.io.DataOutputStream d = new java.io.DataOutputStream(bo);
        d.writeInt(0xCAFEBABE);
        d.writeShort(0); d.writeShort(52);          // version: Java 8
        d.writeShort(5);                            // constant_pool_count (1..4)
        d.writeByte(7); d.writeShort(2);            // #1 Class -> #2
        d.writeByte(1); d.writeUTF(internalName);   // #2 Utf8 this-class name
        d.writeByte(7); d.writeShort(4);            // #3 Class -> #4
        d.writeByte(1); d.writeUTF("java/lang/Object");  // #4 Utf8 super
        d.writeShort(0x0021);                       // ACC_PUBLIC | ACC_SUPER
        d.writeShort(1); d.writeShort(3);           // this_class=#1, super_class=#3
        d.writeShort(0); d.writeShort(0); d.writeShort(0); d.writeShort(0);  // ifaces/fields/methods/attrs
        d.flush();
        return bo.toByteArray();
    }

    public static void main(String[] args) throws Exception
    {
        Greeter g = who -> "Hello, " + who + "!";
        scores.put("player", 100);
        for (int n = 0; n < 40; n++) live.add(new Worker(n, "worker-" + n, n));

        System.out.println("[ExampleApp] JVM up — attach the vmhook viewer to this process.");
        System.out.println(g.greet("vmhook") + " color=" + defaultColor
                           + " instances=" + live.size());

        long i = 0;
        while (true)
        {
            ExampleApp.tick();
            // Mutate every live instance's own fields so the viewer shows them change.
            for (int n = 0; n < live.size(); n++)
            {
                ExampleApp a = live.get(n);
                a.ticks  = (int) (i + n);
                a.label  = "worker-" + n + "#" + i;
                a.active = ((i + n) % 2 == 0);
                a.compute((int) i, ratio);
            }
            history[(int) (i % history.length)] = i;
            // Load one new JDK class per tick so the viewer's re-scan detects
            // runtime-added (bootstrap) classes (see LAZY_CLASSES).
            if (i < LAZY_CLASSES.length)
            {
                try { Class.forName(LAZY_CLASSES[(int) i]); }
                catch (Throwable ignored) { }
            }
            // DEFINE a brand-new class every tick via ClassLoader.defineClass so
            // the viewer's on_class_loaded hook catches it live.
            try { RUNTIME_LOADER.define("com.example.demo.RuntimeGen" + i, genClass("com/example/demo/RuntimeGen" + i)); }
            catch (Throwable ignored) { }
            Thread.sleep(1000);
            i++;
        }
    }
}
