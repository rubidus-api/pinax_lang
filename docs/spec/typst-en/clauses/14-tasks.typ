#import "/tools/common.typ": *

= Tasks <tasks>

#part("Syntax")
```ebnf
tasks-decl = "tasks" NAME size [ size [ size [ size ] ] ] [ "apart" ] ;   (* count, call depth, region bytes, turn limit *)
```

#para("1")[A *task* is a flow of execution that takes turns with others inside one state. Tasks change only where one
gives way on its own (`yield`, or a host word that waits). So even when two tasks use the same global, no other task
comes in between two yields of one task. Tasks are not threads: a state runs on one thread at a time (5.1).]

#para("2")[`tasks NAME count [depth [region [turns]]]` declares a #term("task group") of up to `count` tasks. Each task
has its own call depth (`depth`) and its own region (`region` bytes); left out, they are the processor's defaults.
`turns`, when it is written, is the group's turn limit (14.6). The name of
the group is the type of the values that refer to its tasks. Written last, `apart` makes a fault inside one of the
group's tasks end that task only (14.4 (3a)); `apart` is a word of the declaration only in that place.]

== Making and running tasks <tasks-run>

#para("1")[`block GROUP spawn` makes one task from a block value (10.5) that takes and gives nothing, and gives the task
(`or fail`, `err.full` when there is no free slot). A new task is stopped and has not run yet.]

#para("2")[`GROUP run_all` runs the tasks of the group in turn until all have ended. A task runs until it yields or
ends; then the next one goes on.]

#ex("spec", "14-tasks.pnx")

#para("3")[`TASK resume` runs that one task up to its next yield or its end, and gives `true` when it has ended.
`TASK is_done` asks whether it has ended. The slot of an ended task is used again by `spawn`.]

#ex("spec", "14-resume.pnx")

#para("4")[Tasks stay in the state. When a run the host started ends, a stopped task is still there and can go on in
the next run.]

#ex("spec", "14-full.pnx")

== Yielding <tasks-yield>

#para("1")[`yield` passes the turn to another task. It can fail (`or fail`): when the task was cancelled it fails with
`err.cancelled`. In the main run, which is not a task, `yield` does nothing and succeeds at once.]

#para("2")[Tasks share data through globals, queues and lists. A full or empty queue is a reason to yield.]

#ex("spec", "14-queue.pnx")

#para("3")[Which words may yield (directly or in a word they call) is known at load. `wait_take` and `wait_put` (14.3)
yield too. Such words cannot be run from inside a host word (clause 19).]

== Waiting for a queue <tasks-queue-wait>

#para("1")[`QUEUE wait_take` takes the front value of a queue as `take` does, but while the queue is empty it yields and
looks again when its turn comes back. `v QUEUE wait_put` puts like `put`, yielding while the queue is full. Both can
fail (`or fail`): a task cancelled while it waits fails with `err.cancelled`. When the value can be taken or put at
once, cancelling is not looked at.]

#ex("spec", "14-queue-wait.pnx")

#para("2")[A task that waits for a queue does not run again until some queue has changed. When `run_all` finds that
every task left waits for a queue and none waits for the host, nothing can change a queue any more: that is the fault
`deadlock`. Like a fault inside a task of a group without `apart` (14.4), it ends every task, those of `apart`
groups too. While a task waits for the host, the state waits
(`PINAX_WAITING`) as before; what that task does when it goes on can free the others.]

#ex("spec", "14-deadlock.pnx")

#para("3")[The main run is not a task and has no one to give its turn to. In the main run, `wait_take` on an empty
queue and `wait_put` on a full one are the fault `deadlock` at once.]

#ex("spec", "14-deadlock-main.pnx")

#para("4")[Only these two words are watched for a deadlock. Tasks that wait in a loop of their own (`loop … yield …
end`), or a task resumed by hand with `resume`, are not; a host that runs such code gives it a budget (19.5).]

== Cancelling <tasks-cancel>

#para("1")[`TASK cancel` tells a task it is cancelled: its next `yield`, or its wait for a queue (14.3), fails with
`err.cancelled`. The task takes that
failure and ends in its own way. Cancelling does not stop a task by force.]

#ex("spec", "14-cancel.pnx")

#part("Constraints")

#para("2")[The block given to `spawn` is a block value and does not see the locals around it. Values reach a task
through globals and queues.]
#ex("spec", "14-spawn-refused.pnx")

#para("2a")[The block of `spawn` takes nothing and gives nothing. A block with parameters is refused.]
#ex("spec", "14-spawn-block.pnx")

#part("Faults")

#para("3")[A fault inside a task of a group without `apart` ends the whole run and every task. The fault goes to the
host like a fault of the main run. The state remains and takes the next run.]
#ex("spec", "14-fault.pnx")

#para("3a")[A fault inside a task of a group declared `apart` ends that task only. Its slot is free again, and
whoever resumed it goes on as after the task's end: `run_all` takes the next task, `resume` gives `true`. What the
task stored until then stays. The run goes on; the host learns of the fault through `pinax_task_faults` (19.10 (3)).]
#ex("spec", "14-apart.pnx")

#para("4")[Two tasks using the same region are not followed by the check of 12.3. Each task has a region of its own, so
this does not normally happen.]

#part("Diagnostics")
#diag(
  ([a block with parameters or results given to `spawn`], "spawn takes a block that takes and gives nothing, and a set of tasks: do ... end workers spawn"),
  ([`resume`, `is_done` or `cancel` on a value that is not a task], "this word takes a task"),
  ([`run_all` on a collection that is not a task group (an array, a list)], "run_all takes a set of tasks"),
  ([a `tasks` count above 4096], "a set holds at most 4096 tasks"),
  ([a call depth of a task outside 1 to 4096], "the call depth of a task is 1 to 4096"),
  ([a region of a task above 16 MiB], "the region of a task is at most 16 MiB"),
  ([`tasks` in a data source], "a data source runs no code and declares no tasks"),
  ([`task_tag` on a value that is not an `int`], "task_tag takes an int"),
  ([a turn limit below 1], "the turn limit of a task is at least 1"),
)

== Naming a task <tasks-tag>

#para("1")[`n task_tag` (`int gives`) gives the running task a #term("tag"): an `int` the host is told with the
task's fault when its group is declared `apart` (14.4 (3a), 19.10 (3)). A task's tag is 0 until it gives one; a later
`task_tag` replaces it. In the main run, which is not a task, the word does nothing. The tag is a part of the task,
and an image keeps it.]

#para("2")[A task learns what it serves after `spawn`, from a queue or a global (14.4 (2)), so it names itself
then. A host that gives each request a number tells its tasks the number, and the task gives it as its tag.]
#ex("spec", "14-tag.pnx")

== Turn limits <tasks-turns>

#para("1")[A #term("turn") of a task lasts from the moment it gets the run (from `run_all` or `resume`) until it yields,
waits or ends. A group declared with a turn limit (14 (2)) allows each turn of its tasks at most that many
instructions, counted as the budget counts them (19.5): the cost of each check at a back branch or a call, and the
bytes scanned. The count starts again at every turn.]

#para("2")[A task that runs past its turn limit faults with `cancelled` at that point. It cannot take the fault as a
failure, since nothing there is written to take one. In a group declared `apart` the task ends alone (14.4 (3a)), and
the host is told as of any such fault (19.10 (3a)). In a group without `apart` the fault ends the run, as any fault
inside a task does (14.4 (3)). So a task that loops without yielding is stopped without stopping the others.]
#ex("spec", "14-turns.pnx")

#para("3")[The limit and the budget are counted apart. When the budget runs out inside a turn, the run is suspended
and the turn goes on with what was left of it when the host resumes the run. What the budget still held then, less
than the cost of the check it ran out at, counts against the turn as used. A task another task resumes runs on
the turn of the one that resumed it, and counts against it too.]
