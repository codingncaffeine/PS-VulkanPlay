using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.VisualTree;
using PSVulkanPlay.Core;

namespace PSVulkanPlay.Views;

/// <summary>
/// Moves through a window with a controller's presses: a direction takes the focus to the
/// control that lies that way (the framework's own search), or changes the value of a slider or
/// a choice it stands on; "accept" presses what has the focus. The focus is given the way the
/// arrow keys give it, so the ring around the focused control shows.
/// </summary>
public sealed class ControllerNavigator(TopLevel window, Func<Control?> page)
{
    // The control this navigator gave the focus to last: on it the ring is showing.
    private Control? _ringOn;
    // The control the focus was put on without a press that led there, until a press moves on from it.
    private Control? _begunAt;
    // What "accept" pressed last on this page: where the focus returns to when it can be pressed
    // again (Play, once the session it started has ended).
    private Control? _pressed;

    /// <summary>The controls a controller stops at.</summary>
    public static bool IsStop(Control control) => control is Button or ComboBox or Slider or CheckBox or TextBox;

    private static bool Usable(Control control) => control.Focusable && control.IsEffectivelyVisible && control.IsEffectivelyEnabled;

    /// <summary>The control that has the focus, when it is one that can be seen and used in this window.</summary>
    public Control? Focused =>
        window.FocusManager?.GetFocusedElement() is Control control && Usable(control) && ReferenceEquals(TopLevel.GetTopLevel(control), window) ? control : null;

    /// <summary>A direction or "accept". The first press only shows where the focus is.</summary>
    public void Press(ControllerPress press)
    {
        Control? focused = Focused;
        if (focused is null)
        {
            ContentChanged();
            return;
        }
        _begunAt = null;
        if (press == ControllerPress.Accept)
        {
            // What was given the focus by a pointer shows no ring: first the ring, then the press.
            if (!ReferenceEquals(focused, _ringOn)) Take(focused);
            else
            {
                _pressed = focused;
                Activate(focused);
            }
            return;
        }
        if (press is ControllerPress.Left or ControllerPress.Right && Adjust(focused, press == ControllerPress.Right ? 1 : -1))
        {
            Take(focused);
            return;
        }
        NavigationDirection? direction = press switch
        {
            ControllerPress.Up => NavigationDirection.Up,
            ControllerPress.Down => NavigationDirection.Down,
            ControllerPress.Left => NavigationDirection.Left,
            ControllerPress.Right => NavigationDirection.Right,
            _ => null,
        };
        if (direction is null) return;
        // Nothing that way: the focus stays, with its ring.
        Take(Next(focused, direction.Value) ?? focused);
    }

    /// <summary>The control a direction leads to from another, or none: the framework's search, kept to what a controller stops at.</summary>
    public Control? Next(Control from, NavigationDirection direction)
    {
        Control? found = Search(from, direction);
        if (direction is not (NavigationDirection.Up or NavigationDirection.Down)) return found;
        // The search sees only what is in view: on a page taller than the window, what lies beyond its edge
        // is brought in a screenful at a time and looked for again, before the focus leaves the page (for
        // the window's own buttons above it, which are always in view).
        if (from.FindAncestorOfType<ScrollViewer>() is not { } scroll) return found;
        if (found is not null && found.FindAncestorOfType<ScrollViewer>() == scroll) return found;
        Vector before = scroll.Offset;
        for (int screenfuls = 0; screenfuls < 20; screenfuls++)
        {
            double step = Math.Max(40, scroll.Viewport.Height * 0.8);
            double bottom = Math.Max(0, scroll.Extent.Height - scroll.Viewport.Height);
            double y = direction == NavigationDirection.Up ? Math.Max(0, scroll.Offset.Y - step) : Math.Min(bottom, scroll.Offset.Y + step);
            if (Math.Abs(y - scroll.Offset.Y) < 0.5) break;
            scroll.Offset = new Vector(scroll.Offset.X, y);
            scroll.UpdateLayout();
            if (Search(from, direction) is { } beyond && beyond.FindAncestorOfType<ScrollViewer>() == scroll) return beyond;
        }
        scroll.Offset = before;
        scroll.UpdateLayout();
        return found;
    }

    private Control? Search(Control from, NavigationDirection direction)
    {
        var options = new FindNextElementOptions { FocusedElement = from, SearchRoot = window };
        for (Control? found = window.FocusManager?.FindNextElement(direction, options) as Control; found is not null; found = found.GetVisualParent() as Control)
        {
            if (IsStop(found)) return Usable(found) && !ReferenceEquals(found, from) ? found : null;
        }
        return null;
    }

    /// <summary>A slider or a choice takes left and right as "less" and "more"; nothing else does.</summary>
    public static bool Adjusts(Control control) => control is Slider or ComboBox;

    private static bool Adjust(Control control, int by)
    {
        switch (control)
        {
            case Slider slider:
                double step = slider.IsSnapToTickEnabled && slider.TickFrequency > 0 ? slider.TickFrequency : slider.SmallChange;
                slider.Value = Math.Clamp(slider.Value + by * step, slider.Minimum, slider.Maximum);
                return true;
            case ComboBox choice:
                if (choice.ItemCount > 0) choice.SelectedIndex = Math.Clamp(choice.SelectedIndex + by, 0, choice.ItemCount - 1);
                return true;
            default:
                return false;
        }
    }

    private static void Activate(Control control)
    {
        switch (control)
        {
            case ComboBox choice:
                // The next choice, and after the last the first again: no list opens over the page.
                if (choice.ItemCount > 0) choice.SelectedIndex = (choice.SelectedIndex + 1) % choice.ItemCount;
                break;
            case Button:
                // The key a keyboard presses a button with, said to come from a controller: the
                // button's own road from a press to its command.
                control.RaiseEvent(new KeyEventArgs { RoutedEvent = InputElement.KeyDownEvent, Key = Key.Enter, KeyDeviceType = KeyDeviceType.Gamepad, Source = control });
                control.RaiseEvent(new KeyEventArgs { RoutedEvent = InputElement.KeyUpEvent, Key = Key.Enter, KeyDeviceType = KeyDeviceType.Gamepad, Source = control });
                break;
        }
    }

    /// <summary>The focus is somewhere a controller can go on from; if it is not, it goes to where the page begins.</summary>
    public void EnsureFocus()
    {
        if (Focused is { } focused) { if (!ReferenceEquals(focused, _ringOn)) Take(focused); }
        else ContentChanged();
    }

    /// <summary>
    /// Where a page begins: the control marked as its first, else the one thing the page is for,
    /// else the first control on it.
    /// </summary>
    public void FocusFirst()
    {
        _pressed = null;
        Begin();
    }

    private void Begin()
    {
        if (page() is not { } root) return;
        List<Control> stops = root.GetVisualDescendants().OfType<Control>().Where(control => IsStop(control) && Usable(control)).ToList();
        Control? first = stops.FirstOrDefault(control => control.Classes.Contains("first")) ?? stops.FirstOrDefault(control => control.Classes.Contains("primary")) ?? stops.FirstOrDefault();
        Take(first);
        _begunAt = first;
    }

    /// <summary>
    /// What is on the page changed without a press: what the page is for arrived after the page
    /// (the consoles, once they have been asked for), or what was pressed cannot be pressed for now
    /// (Play, while its session runs) or can be again. Where no press has moved the focus since
    /// it was last put somewhere, it goes back to what was pressed, or to where the page begins.
    /// </summary>
    public void ContentChanged()
    {
        Control? focused = Focused;
        if (focused is not null && !ReferenceEquals(focused, _begunAt)) return;
        if (_pressed is { } pressed && IsStop(pressed) && Usable(pressed) && ReferenceEquals(TopLevel.GetTopLevel(pressed), window))
        {
            Take(pressed);
            _begunAt = pressed;
        }
        else Begin();
    }

    private void Take(Control? control)
    {
        if (control is null) return;
        _ringOn = control;
        // A control that has the focus from a pointer has it without a ring, and being given the
        // focus it already has changes nothing: it lets go of it first.
        if (control.IsFocused && !control.Classes.Contains(":focus-visible")) window.FocusManager?.Focus(null, NavigationMethod.Unspecified, KeyModifiers.None);
        control.Focus(NavigationMethod.Directional);
        // With room around it: what a control belongs to (its row, its heading) comes into view with it.
        control.BringIntoView(new Rect(control.Bounds.Size).Inflate(new Thickness(0, 70)));
    }
}
