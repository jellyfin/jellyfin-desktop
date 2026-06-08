//! Pure state machine for the context menu's open sequence.
//!
//! [`LifeEffect::Fire`] is emitted on exactly one transition into
//! [`Phase::Closed`]. This covers only the exit paths inside `run_menu`; the
//! pre-dispatch failures in `show()` happen before any [`Life`] exists and fire
//! their callback directly.

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Phase {
    Idle,
    Building,
    Grabbing,
    Active,
    Closed,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LifeEvent {
    Show,
    BuildOk,
    BuildFail,
    GrabOk,
    GrabFail,
    /// Result from the interaction loop: command id, or `-1` to dismiss.
    Result(i32),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LifeEffect {
    Build,
    Grab,
    Cleanup,
    Fire(i32),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Life {
    pub phase: Phase,
}

impl Default for Life {
    fn default() -> Self {
        Self { phase: Phase::Idle }
    }
}

pub fn step(life: &mut Life, ev: &LifeEvent) -> Vec<LifeEffect> {
    match (life.phase, *ev) {
        (Phase::Idle, LifeEvent::Show) => {
            life.phase = Phase::Building;
            vec![LifeEffect::Build]
        }
        (Phase::Building, LifeEvent::BuildOk) => {
            life.phase = Phase::Grabbing;
            vec![LifeEffect::Grab]
        }
        (Phase::Building, LifeEvent::BuildFail) => {
            life.phase = Phase::Closed;
            vec![LifeEffect::Fire(-1), LifeEffect::Cleanup]
        }
        (Phase::Grabbing, LifeEvent::GrabOk) => {
            life.phase = Phase::Active;
            vec![]
        }
        (Phase::Grabbing, LifeEvent::GrabFail) => {
            life.phase = Phase::Closed;
            vec![LifeEffect::Fire(-1), LifeEffect::Cleanup]
        }
        (Phase::Active, LifeEvent::Result(id)) => {
            life.phase = Phase::Closed;
            vec![LifeEffect::Fire(id), LifeEffect::Cleanup]
        }
        _ => vec![],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(phase: Phase, ev: LifeEvent) -> (Phase, Vec<LifeEffect>) {
        let mut life = Life { phase };
        let e = step(&mut life, &ev);
        (life.phase, e)
    }

    #[test]
    fn show_starts_build() {
        assert_eq!(
            run(Phase::Idle, LifeEvent::Show),
            (Phase::Building, vec![LifeEffect::Build])
        );
    }

    #[test]
    fn build_ok_grabs() {
        assert_eq!(
            run(Phase::Building, LifeEvent::BuildOk),
            (Phase::Grabbing, vec![LifeEffect::Grab])
        );
    }

    #[test]
    fn build_fail_closes_dismiss() {
        assert_eq!(
            run(Phase::Building, LifeEvent::BuildFail),
            (
                Phase::Closed,
                vec![LifeEffect::Fire(-1), LifeEffect::Cleanup]
            )
        );
    }

    #[test]
    fn grab_ok_actives() {
        assert_eq!(
            run(Phase::Grabbing, LifeEvent::GrabOk),
            (Phase::Active, vec![])
        );
    }

    #[test]
    fn grab_fail_closes_dismiss() {
        assert_eq!(
            run(Phase::Grabbing, LifeEvent::GrabFail),
            (
                Phase::Closed,
                vec![LifeEffect::Fire(-1), LifeEffect::Cleanup]
            )
        );
    }

    #[test]
    fn result_fires_once_and_closes() {
        assert_eq!(
            run(Phase::Active, LifeEvent::Result(7)),
            (
                Phase::Closed,
                vec![LifeEffect::Fire(7), LifeEffect::Cleanup]
            )
        );
    }

    #[test]
    fn illegal_transitions_noop() {
        assert_eq!(run(Phase::Idle, LifeEvent::GrabOk), (Phase::Idle, vec![]));
        assert_eq!(run(Phase::Closed, LifeEvent::Show), (Phase::Closed, vec![]));
        assert_eq!(run(Phase::Active, LifeEvent::Show), (Phase::Active, vec![]));
        assert_eq!(
            run(Phase::Building, LifeEvent::Result(3)),
            (Phase::Building, vec![])
        );
    }
}
