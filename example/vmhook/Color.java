package vmhook;

// Enum probe target.  Verifies that vmhook can:
//   - Read enum value references (each enum constant is a singleton
//     object with its own OOP).
//   - Distinguish between enum ordinal and the backing String name.
//   - Read fields declared on the enum body.
public enum Color
{
    RED   (0xFF0000),
    GREEN (0x00FF00),
    BLUE  (0x0000FF);

    public final int rgb;

    Color(final int rgb)
    {
        this.rgb = rgb;
    }

    // Instance method on enum.  Verifies that vmhook can call enum
    // instance methods through the same proxy machinery as regular
    // classes.
    public int brightness()
    {
        return ((this.rgb >> 16) & 0xFF)
             + ((this.rgb >>  8) & 0xFF)
             + ((this.rgb      ) & 0xFF);
    }
}
