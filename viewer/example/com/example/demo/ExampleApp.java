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
            Thread.sleep(1000);
            i++;
        }
    }
}
