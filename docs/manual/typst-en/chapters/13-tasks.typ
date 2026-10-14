#import "/tools/common.typ": *

= Tasks <tasks>

#opening[Chapter 4 (blocks), chapter 6 (queues), chapter 8 (faults).][
- How does one state take turns at several jobs?
- When do tasks change, and how do they pass data?
- What happens when one task has a fault?
]

== Taking turns

A #strong[task] is a flow of execution inside one state. It is not a thread: tasks change only where a task `yield`s
on its own. So nothing comes in between two `yield`s, and no locks are needed.

`tasks NAME count` declares a task group, `block GROUP spawn` makes a task, and `GROUP run_all` runs them until all have
ended.

#ex("manual", "13-turns.pnx")

== Linking with queues

Tasks pass data through declared storage, most often queues. When a queue is full or empty, a task `yield`s and waits
for its next turn.

#ex("manual", "13-pipeline.pnx")

Each of the three stages is a task, and the queues hold only two values each. The stages wait for each other and take
turns. The end is announced by sending `-1`.

#mistake[The block of `spawn` sees the locals around it][The block given to `spawn` is a block value and does not see
the locals around it, because the task may live on after that word has ended. To give a task values, use globals or
queues, or --- as in the examples above --- call a word inside the block with literals.
#ex("manual", "13-no-locals.pnx")]

== Faults and cancelling

A fault in one task ends the whole run --- the other tasks stop too. The state remains and takes the next run.

#ex("manual", "13-fault.pnx")

`TASK cancel` makes that task's next `yield` fail with `cancelled`. The task takes the failure with `or_else`, tidies up
and ends. Cancelling does not stop a task by force.

#summary[
- Tasks change only where they `yield` (standard 14.2).
- `spawn` makes a task from a block that takes and gives nothing; `run_all` runs them (14.1).
- Tasks share data through globals and queues; blocks do not see the locals around them (14.3).
- A fault in a task ends the whole run (14.3).
]
