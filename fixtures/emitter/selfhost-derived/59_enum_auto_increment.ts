enum Direction {
  North,
  East,
  South,
  West,
}

enum HttpStatus {
  OK = 200,
  NotFound = 404,
  ServerError = 500,
}

const d: Direction = Direction.East;
const code: HttpStatus = HttpStatus.OK;
