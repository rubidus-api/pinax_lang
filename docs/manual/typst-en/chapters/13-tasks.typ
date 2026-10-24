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

One `spawn` line, followed, shows what a task takes and gives.

#trace(
  ("do \"ping\" 3 count_to end", "block", [A block that takes and gives nothing goes on as a value; the literals inside belong to it.]),
  ("workers spawn", "task", [Makes one task in the set and gives it. Fails when no place is free.]),
  ([`or_else (e) return end`], "task", [Success: skipped. On failure the word is left.]),
  ("drop", "—", [The task value serves `resume` and `cancel`; here `run_all` runs it, so it is dropped.]),
)

`spawn` only makes a task; it does not run it. `run_all` gives each task of the set one turn after another, and a task
gives its turn up at `yield`. Turn by turn, this is why the output alternates.

#table(columns: (auto, auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Turn*], [*Task*], [*Prints*], [*Then*]),
  [1], [ping], [`ping 1`], [`yield`: gives the turn up],
  [2], [pong], [`pong 1`], [`yield`],
  [3], [ping], [`ping 2`], [`yield`],
  [4], [pong], [`pong 2`], [`yield`],
  [5], [ping], [`ping 3`], [`yield`],
  [6], [pong], [(nothing)], [`i` is 3: `until` leaves the loop and the task ends],
  [7], [ping], [(nothing)], [`i` is 4: the task ends. All have ended, so `run_all` returns],
)

Each task has its own locals (`name`, `n`, `i`) and its own stack. So after going away at `yield` and coming back, it
goes on exactly where it was.

== Linking with queues

Tasks pass data through declared storage, most often queues. `q wait_take` takes from a queue and `v q wait_put` puts
into one; while the queue is empty (or full) the task yields and looks again on its next turn.

#ex("manual", "13-pipeline.pnx")

Each of the three stages is a task, and the queues hold only two values each. The stages wait for each other and take
turns. The end is announced by sending `-1`.

When every task waits for a queue that none of them will fill, nothing can go on. `run_all` sees that and ends the run
with the fault `deadlock`, instead of turning forever. In the main run, which is not a task, waiting for a queue is
that fault at once.

#ex("manual", "13-deadlock.pnx")

Only the two waiting words are watched. A task that waits in a loop of its own (`loop … yield … end`) is not, so a host
that runs such code gives it a budget (chapter 14).

#mistake[The block of `spawn` sees the locals around it][The block given to `spawn` is a block value and does not see
the locals around it, because the task may live on after that word has ended. To give a task values, use globals or
queues, or --- as in the examples above --- call a word inside the block with literals.
#ex("manual", "13-no-locals.pnx")]

== Faults and cancelling

A fault in one task ends the whole run --- the other tasks stop too. The state remains and takes the next run.

#ex("manual", "13-fault.pnx")

When the tasks do work that must not take the others down with it (requests of a server, jobs from a queue), declare
the group `apart`. Then a fault ends only the task that has it; the others go on, and so does the run.

#ex("manual", "13-apart.pnx")

`TASK cancel` makes that task's next `yield`, or its wait for a queue, fail with `cancelled`. The task takes the failure with `or_else`, tidies up
and ends. Cancelling does not stop a task by force.

#qa[How many tasks can there be?][As many as the `count` of `tasks NAME count`, at most 4096. The place of a task that
ended is used again. Each task has a call depth and a region of its own, so a large count needs a large block.]

#qa[What does `yield` do in the main run?][Nothing, and it succeeds. The main run is not a task and has nobody to give
a turn to. So the same word serves inside a task and outside.]

#summary[
- Tasks change only where they `yield` (standard 14.2).
- `spawn` makes a task from a block that takes and gives nothing; `run_all` runs them (14.1).
- Tasks share data through globals and queues; `wait_take` and `wait_put` wait for a queue, and a run where every
  task waits for one is the fault `deadlock` (14.3).
- Blocks given to `spawn` do not see the locals around them (14.4).
- A fault in a task ends the whole run (14.4), or only that task when its group is declared `apart` (14.4 (3a)).
]
