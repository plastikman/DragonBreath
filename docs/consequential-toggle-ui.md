# Consequential toggle UI prototype

DragonBreath's `/diag` page prototypes a presentation-only pattern for boolean
changes with an operational consequence. The prototype currently guards enabling
automatic filtration: enabling it may start the chamber blower later when the
saved bed-temperature condition becomes true. Disabling it is immediate and does
not need acknowledgement.

The browser flow is deliberately narrow:

1. Read authoritative state from the product.
2. Restore the visible toggle to that committed value before asking anything.
3. Ask the product-owned `confirm()` callback whether this requested value needs
   acknowledgement.
4. Cancel without a mutation, or call the product-owned `mutate()` callback once.
5. Render only the state returned by the product. On failure, read authoritative
   state again and show the error.

The component owns presentation, focus, pending state, duplicate suppression and
server reconciliation. DragonBreath still owns the setting, eligibility, text,
side effects, persistence and validation. The modal is not a safety boundary;
firmware must validate every request without trusting the browser.

## Smallest plausible future `dc_ui` API

The stable board-neutral surface appears to be one mount function:

```js
mountConsequentialToggle(element, {
  id,
  label,
  description,
  read: () => Promise<{ value, disabled, disabledReason }>,
  confirm: (requested, current) => null | {
    title, body, cancelLabel, confirmLabel, severity
  },
  mutate: requested => Promise<{ value, disabled, disabledReason }>,
  pendingMessage,
  successMessage,
  failureMessage,
  onState
})
```

`dc_ui` could own the semantic switch, native dialog, inert backdrop, focus rules,
pending/error rendering and generic callback wiring. It should not know endpoint
paths, controller names, printer brands, safety conditions, restoration policy or
what another setting means.

This is intentionally not a schema framework. If later Dragon products need
server-described controls, that should be designed from multiple downstream uses
rather than inferred from this single prototype.

## Jump Jet fit

Jump Jet could supply a confirmation explaining that Automatic mode follows
PrusaLink printer state and bed target, while retaining a Manual target for later
restoration. Jump Jet firmware would still perform one validated mode transition
and return its authoritative result. No Jump Jet string or policy belongs in this
component.

The abstraction is sufficiently product-neutral to test downstream, but it has
only one real product integration so far. It should not move into dragon-core until
a second product validates the callback and state shapes.
