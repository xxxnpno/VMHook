package com.example.demo;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * A long-running example JVM to test the vmhook viewer against.
 *
 * It deliberately exposes a rich surface — static and instance fields of many
 * types, methods with varied signatures, a nested class, an interface and an
 * enum — so that after you attach the viewer you can find
 * {@code com/example/demo/ExampleApp} and see its members populated.
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

    // ── instance fields (various types) ──────────────────────────────────────
    public              int            id;
    public              int            ticks;   // mutated every second (live demo)
    private             String         label;
    protected           boolean        active;
    float[]                            samples;
    List<String>                       tags;
    Inner                              inner;

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

    // ── entry point (runs forever so it stays attachable) ────────────────────
    public static void main(String[] args) throws Exception
    {
        ExampleApp app = new ExampleApp(1, "main");
        Greeter    g   = who -> "Hello, " + who + "!";
        scores.put("player", 100);

        System.out.println("[ExampleApp] JVM up — attach the vmhook viewer to this process.");
        System.out.println(g.greet("vmhook") + " color=" + defaultColor + " inner=" + app.inner.doubleIt(21));

        long i = 0;
        while (true)
        {
            ExampleApp.tick();
            // Mutate the live instance's own fields so the viewer shows them change.
            app.ticks  = (int) i;
            app.label  = "main#" + i;
            app.active = (i % 2 == 0);
            history[(int) (i % history.length)] = i;
            app.compute((int) i, ratio);
            Thread.sleep(1000);
            i++;
        }
    }
}
