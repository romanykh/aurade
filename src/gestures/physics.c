#include "physics.h"
#include <math.h>

spring_config_t spring_config_default(void) {
    return (spring_config_t){
        .mass = 1.0,
        .stiffness = 280.0,
        .damping = 0.92,
        .velocity_threshold = 0.5,
        .position_threshold = 0.001,
    };
}

void spring_simulation_start(spring_simulation_t *sim, const spring_config_t *config,
                             double start_pos, double target_pos, double initial_velocity) {
    sim->config = config ? *config : spring_config_default();
    sim->start_pos = start_pos;
    sim->target_pos = target_pos;
    sim->initial_velocity = initial_velocity;
    sim->current_pos = start_pos;
    sim->current_vel = initial_velocity;
    sim->elapsed_time = 0.0;
    sim->settled = (fabs(start_pos - target_pos) < sim->config.position_threshold &&
                    fabs(initial_velocity) < sim->config.velocity_threshold);
}

bool spring_simulation_step(spring_simulation_t *sim, double dt) {
    if (sim->settled) {
        sim->current_pos = sim->target_pos;
        sim->current_vel = 0.0;
        return false;
    }

    sim->elapsed_time += dt;
    double t = sim->elapsed_time;

    double m = sim->config.mass;
    if (m <= 0.0) m = 1.0;
    double k = sim->config.stiffness;
    double zeta = sim->config.damping;
    double omega0 = sqrt(k / m);
    double delta_x0 = sim->start_pos - sim->target_pos;
    double v0 = sim->initial_velocity;

    double x_rel = 0.0;
    double v = 0.0;

    if (zeta < 0.9999) {
        // Underdamped (subtle macOS bounce)
        double omega_d = omega0 * sqrt(1.0 - zeta * zeta);
        double decay = exp(-zeta * omega0 * t);
        double c1 = delta_x0;
        double c2 = (v0 + zeta * omega0 * delta_x0) / omega_d;

        double cos_val = cos(omega_d * t);
        double sin_val = sin(omega_d * t);

        x_rel = decay * (c1 * cos_val + c2 * sin_val);
        v = -zeta * omega0 * decay * (c1 * cos_val + c2 * sin_val) +
            decay * (-c1 * omega_d * sin_val + c2 * omega_d * cos_val);
    } else if (zeta > 1.0001) {
        // Overdamped
        double omega_star = omega0 * sqrt(zeta * zeta - 1.0);
        double r1 = -zeta * omega0 + omega_star;
        double r2 = -zeta * omega0 - omega_star;

        double c2 = (v0 - r1 * delta_x0) / (r2 - r1);
        double c1 = delta_x0 - c2;

        double exp1 = exp(r1 * t);
        double exp2 = exp(r2 * t);

        x_rel = c1 * exp1 + c2 * exp2;
        v = c1 * r1 * exp1 + c2 * r2 * exp2;
    } else {
        // Critically damped (zeta == 1.0)
        double decay = exp(-omega0 * t);
        double c1 = delta_x0;
        double c2 = v0 + omega0 * delta_x0;

        x_rel = decay * (c1 + c2 * t);
        v = decay * (c2 - omega0 * (c1 + c2 * t));
    }

    sim->current_pos = sim->target_pos + x_rel;
    sim->current_vel = v;

    // Check if settled
    if (fabs(x_rel) < sim->config.position_threshold &&
        fabs(v) < sim->config.velocity_threshold) {
        sim->current_pos = sim->target_pos;
        sim->current_vel = 0.0;
        sim->settled = true;
        return false;
    }

    return true;
}

double rubber_band_calculate(double displacement, double limit_constant) {
    if (limit_constant <= 0.0) limit_constant = 200.0;
    double sign = displacement >= 0.0 ? 1.0 : -1.0;
    double abs_d = fabs(displacement);
    return sign * (abs_d / (1.0 + (abs_d / limit_constant)));
}
