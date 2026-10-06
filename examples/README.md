# Examples

Pages that exercise OpenQBrowser's engine. Each one is a plain HTML file with no
build step: open it with the browser and read the source beside the result.

```bash
# From the repository root, after building:
./build/bin/openqbrowser --screenshot=out.png "file://$PWD/examples/basic-layout.html"
```

| File | What it shows |
| --- | --- |
| `basic-layout.html` | Block stacking, margin collapsing, auto margins, borders and padding |
| `inline-text.html` | Line breaking, font sizes and weights, text alignment, inline-blocks |
| `lists-and-tables.html` | List markers, nested lists, table display types |
| `css-cascade.html` | Specificity, inheritance, `!important`, media queries, presentational attributes |
| `interactive-survey.html` | A realistic page: header, navigation, cards, a form and a footer |
| `scripted-page.html` | Building the DOM, timers, events, `style` and `classList` from page script |

## A note on scripting

OpenQBrowser runs JavaScript, so a page that builds its content in a script
renders that content. `interactive-survey.html` is deliberately written with
plain markup and CSS instead, so the examples show what the renderer does on its
own: the form fields are present and styled, but nothing submits or validates
them. For scripting, see `examples/scripted-page.html`.
