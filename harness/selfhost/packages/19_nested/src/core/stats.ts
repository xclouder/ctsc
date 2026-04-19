import { sum, max } from "../util/num.js";
import { join } from "../util/str.js";

export interface Stats {
  total: number;
  peak: number;
  label: string;
}

export function analyze(values: number[], tags: string[]): Stats {
  return {
    total: sum(values),
    peak: max(values),
    label: join(tags, ","),
  };
}
